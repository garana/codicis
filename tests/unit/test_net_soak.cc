/**
 * @file test_net_soak.cc
 * @brief Load / soak tests for the net stack over the REAL platform event loop
 *        (epoll on Linux, kqueue elsewhere).
 *
 * Where test_net_epoll targets individual edge cases, these drive the
 * single-threaded HttpServer under sustained, concurrent load:
 *   - high concurrency + HTTP/1.1 pipelining (many connections each firing a
 *     long batch of keep-alive requests), and
 *   - connection churn (many short-lived-connection waves), which surfaces fd
 *     or registration leaks that only appear after thousands of accept/close
 *     cycles.
 *
 * All clients are non-blocking and are serviced in lock-step with the server
 * loop on this one thread, so writes and reads always drain and the test never
 * deadlocks on a full socket buffer. A wall-clock deadline guards against a
 * hang turning into a stuck suite.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "codicis/event/event_loop.h"
#include "codicis/net/http_server.h"
#include "codicis/net/router.h"
#include "codicis/util/clock.h"

using namespace codicis;

namespace {

/** @brief Open a non-blocking loopback client connected to @p port, or -1. */
int ConnectLoopbackNonblocking(std::uint16_t port) {
  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  if (c < 0) {
    return -1;
  }
  struct sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (::connect(c, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
    ::close(c);
    return -1;
  }
  const int flags = ::fcntl(c, F_GETFL, 0);
  ::fcntl(c, F_SETFL, flags | O_NONBLOCK);
  return c;
}

/** @brief One driven client: pending request bytes out, response bytes in. */
struct Client {
  int fd = -1;
  std::string out;      /**< Request bytes still to be written. */
  std::size_t sent = 0; /**< How many of @ref out have been written. */
  std::string in;       /**< Response bytes read so far. */
  int responses = 0;    /**< Count of 200 responses observed. */
  bool eof = false;     /**< Server closed its side. */
};

/** @brief Count non-overlapping occurrences of @p needle in @p hay. */
int CountOccurrences(const std::string& hay, const std::string& needle) {
  int count = 0;
  std::size_t pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

/**
 * @brief Run one batch of connections to completion against @p server.
 *
 * Opens @p num_clients connections, each pipelining @p reqs_per_client GET
 * /health requests (the last with Connection: close so the server closes the
 * socket after the final response). Services every client non-blocking while
 * pumping the loop until all have received all their responses and seen EOF.
 * Requires that each client observed exactly @p reqs_per_client responses.
 * @return The total number of 200 responses across the batch.
 */
int RunBatch(EventLoop& loop, HttpServer& server, int num_clients,
             int reqs_per_client) {
  std::string keep = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n";
  std::string last =
      "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

  std::vector<Client> clients(static_cast<std::size_t>(num_clients));
  for (Client& cl : clients) {
    cl.fd = ConnectLoopbackNonblocking(server.port());
    REQUIRE(cl.fd >= 0);
    for (int r = 0; r < reqs_per_client - 1; ++r) {
      cl.out += keep;
    }
    cl.out += last;  // final request closes the connection
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  int done = 0;
  while (done < num_clients) {
    loop.run_once(1);

    done = 0;
    for (Client& cl : clients) {
      if (cl.eof) {
        ++done;
        continue;
      }
      // Push any pending request bytes.
      while (cl.sent < cl.out.size()) {
        const ssize_t n =
            ::send(cl.fd, cl.out.data() + cl.sent, cl.out.size() - cl.sent, 0);
        if (n > 0) {
          cl.sent += static_cast<std::size_t>(n);
          continue;
        }
        break;  // EAGAIN or error: retry on a later iteration
      }
      // Drain any available response bytes.
      for (;;) {
        char buf[8192];
        const ssize_t n = ::recv(cl.fd, buf, sizeof(buf), 0);
        if (n > 0) {
          cl.in.append(buf, static_cast<std::size_t>(n));
          continue;
        }
        if (n == 0) {
          cl.eof = true;  // server closed after the final response
          break;
        }
        break;  // EAGAIN: nothing more for now
      }
      if (cl.eof) {
        ++done;
      }
    }

    REQUIRE(std::chrono::steady_clock::now() < deadline);
  }

  int total = 0;
  for (Client& cl : clients) {
    cl.responses = CountOccurrences(cl.in, "HTTP/1.1 200");
    CHECK(cl.responses == reqs_per_client);
    total += cl.responses;
    ::close(cl.fd);
  }
  return total;
}

/** @brief A router whose /health always returns 200 "ok". */
HttpRouter MakeRouter() {
  HttpRouter router;
  router.add("GET", "/health", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200);
    resp.set_header("Content-Type", "text/plain");
    resp.body = "ok";
  });
  return router;
}

}  // namespace

TEST_CASE("Load: many concurrent connections each pipelining a request batch",
          "[net][epoll][soak]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter();
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  constexpr int kConns = 100;
  constexpr int kReqs = 50;  // 5,000 requests over 100 pipelined connections
  const int total = RunBatch(loop, server, kConns, kReqs);
  REQUIRE(total == kConns * kReqs);

  // Every connection must have been reclaimed.
  for (int i = 0; i < 20; ++i) {
    loop.run_once(1);
  }
  REQUIRE(server.connection_count() == 0);
}

TEST_CASE("Soak: sustained connection churn leaks no connections",
          "[net][epoll][soak]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter();
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  constexpr int kWaves = 40;
  constexpr int kPerWave = 25;  // 1,000 short-lived connections total
  constexpr int kReqs = 4;
  int grand_total = 0;
  for (int w = 0; w < kWaves; ++w) {
    grand_total += RunBatch(loop, server, kPerWave, kReqs);
    // The book-keeping must return to zero after every wave; a steady climb
    // here would mean a per-connection leak in the accept/close cycle.
    for (int i = 0; i < 10; ++i) {
      loop.run_once(1);
    }
    REQUIRE(server.connection_count() == 0);
  }
  REQUIRE(grand_total == kWaves * kPerWave * kReqs);
}
