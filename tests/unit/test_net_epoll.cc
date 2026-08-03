/**
 * @file test_net_epoll.cc
 * @brief Integration tests exercising the net/listener path over the REAL
 *        platform event loop (epoll on Linux, kqueue on BSD/macOS).
 *
 * The existing test_http_server cases cover the happy request/response path.
 * These cases target behaviours that are specific to a level-triggered kernel
 * poller driving non-blocking sockets, and that had never run against epoll:
 *   - a request dribbled in one byte at a time (readable fires many times);
 *   - a large response that cannot be written in one syscall (EPOLLOUT/EVFILT
 *     write re-arm, partial writes, backpressure);
 *   - a client half-close (shutdown(SHUT_WR)) and an abrupt mid-request close
 *     (EPOLLRDHUP/EPOLLHUP vs EV_EOF -> kHangup/peer-closed teardown);
 *   - many simultaneous connections (the accept() drain loop under LT).
 *
 * Everything is driven through MakeEventLoop(), so on Linux this is genuine
 * epoll coverage; the same asserts hold on kqueue.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "codicis/event/event_loop.h"
#include "codicis/net/http_server.h"
#include "codicis/net/listener.h"
#include "codicis/net/router.h"
#include "codicis/util/clock.h"

using namespace codicis;

namespace {

/** @brief Open a blocking loopback client connected to @p port, or -1. */
int ConnectLoopback(std::uint16_t port) {
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
  return c;
}

/** @brief Put a descriptor into non-blocking mode. */
void SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/** @brief Pump the loop up to @p iters times, @p ms per wait. */
void Pump(EventLoop& loop, int iters, int ms) {
  for (int i = 0; i < iters; ++i) {
    loop.run_once(ms);
  }
}

/**
 * @brief Drain the client socket to EOF, pumping the server loop as we go.
 * @return The full response bytes read until the server closed the connection.
 */
std::string ReadToClose(EventLoop& loop, int client) {
  SetNonBlocking(client);
  std::string response;
  bool done = false;
  for (int i = 0; i < 2000 && !done; ++i) {
    loop.run_once(5);
    char buf[8192];
    for (;;) {
      const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
      if (n > 0) {
        response.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) {
        done = true;  // server closed
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;  // nothing pending; pump again
      }
      done = true;
      break;
    }
  }
  return response;
}

/** @brief A router with /health, /echo, and a configurable-size /big. */
HttpRouter MakeRouter(std::size_t big_size) {
  HttpRouter router;
  router.add("GET", "/health", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200);
    resp.set_header("Content-Type", "text/plain");
    resp.body = "ok";
  });
  router.add("GET", "/big", [big_size](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200);
    resp.set_header("Content-Type", "application/octet-stream");
    resp.body = std::string(big_size, 'Z');
  });
  return router;
}

}  // namespace

TEST_CASE("Request dribbled one byte at a time is served",
          "[net][epoll][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter(0);
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const int c = ConnectLoopback(server.port());
  REQUIRE(c >= 0);

  // Pump once so the server accepts the connection.
  Pump(loop, 3, 2);

  const std::string request =
      "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  // Send each byte in its own segment, pumping the loop between bytes so the
  // server's readable handler fires repeatedly on partial input.
  for (char ch : request) {
    REQUIRE(::send(c, &ch, 1, 0) == 1);
    loop.run_once(1);
  }

  const std::string resp = ReadToClose(loop, c);
  ::close(c);

  REQUIRE(resp.rfind("HTTP/1.1 200", 0) == 0);
  REQUIRE(resp.find("ok") != std::string::npos);
}

TEST_CASE("Large response is delivered across partial writes (backpressure)",
          "[net][epoll][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  // 4 MiB dwarfs the socket send/recv buffers, so write() must hit EAGAIN and
  // the connection must re-arm write interest and finish over several
  // writable events.
  const std::size_t kBig = std::size_t{4} * 1024 * 1024;
  HttpRouter router = MakeRouter(kBig);
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const int c = ConnectLoopback(server.port());
  REQUIRE(c >= 0);
  const std::string request =
      "GET /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  REQUIRE(::send(c, request.data(), request.size(), 0) ==
          static_cast<ssize_t>(request.size()));

  const std::string resp = ReadToClose(loop, c);
  ::close(c);

  REQUIRE(resp.rfind("HTTP/1.1 200", 0) == 0);
  const std::size_t hdr_end = resp.find("\r\n\r\n");
  REQUIRE(hdr_end != std::string::npos);
  const std::size_t body_len = resp.size() - (hdr_end + 4);
  REQUIRE(body_len == kBig);  // every byte made it through, none truncated
}

TEST_CASE("Client half-close (SHUT_WR) still gets a response then closes",
          "[net][epoll][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter(0);
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const int c = ConnectLoopback(server.port());
  REQUIRE(c >= 0);

  // Keep-alive request (server would normally hold the connection open), but
  // we half-close our write side right after sending. The server sees EOF on
  // the read half (kHangup/peer-closed) and must still answer, then tear down.
  const std::string request = "GET /health HTTP/1.1\r\nHost: x\r\n\r\n";
  REQUIRE(::send(c, request.data(), request.size(), 0) ==
          static_cast<ssize_t>(request.size()));
  REQUIRE(::shutdown(c, SHUT_WR) == 0);

  const std::string resp = ReadToClose(loop, c);
  ::close(c);

  REQUIRE(resp.rfind("HTTP/1.1 200", 0) == 0);
  // Pump a little more; the server must have dropped the connection.
  Pump(loop, 5, 2);
  REQUIRE(server.connection_count() == 0);
}

TEST_CASE("Abrupt close mid-request tears the connection down cleanly",
          "[net][epoll][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter(0);
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const int c = ConnectLoopback(server.port());
  REQUIRE(c >= 0);
  Pump(loop, 3, 2);
  REQUIRE(server.connection_count() == 1);

  // A partial request with no terminating blank line, then a hard close.
  const std::string partial = "GET /health HTTP/1.1\r\nHost: x\r\n";
  REQUIRE(::send(c, partial.data(), partial.size(), 0) ==
          static_cast<ssize_t>(partial.size()));
  ::close(c);

  // The server must observe EOF and reclaim the connection (no spin, no leak).
  Pump(loop, 20, 2);
  REQUIRE(server.connection_count() == 0);
}

TEST_CASE("Many simultaneous connections are all accepted and served",
          "[net][epoll][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter(0);
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  constexpr int kConns = 48;
  std::vector<int> clients;
  clients.reserve(kConns);
  const std::string request =
      "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  for (int i = 0; i < kConns; ++i) {
    const int c = ConnectLoopback(server.port());
    REQUIRE(c >= 0);
    REQUIRE(::send(c, request.data(), request.size(), 0) ==
            static_cast<ssize_t>(request.size()));
    clients.push_back(c);
  }

  // Drain every client to EOF, pumping the shared loop throughout.
  int ok200 = 0;
  for (const int c : clients) {
    const std::string resp = ReadToClose(loop, c);
    if (resp.rfind("HTTP/1.1 200", 0) == 0 &&
        resp.find("ok") != std::string::npos) {
      ++ok200;
    }
    ::close(c);
  }
  REQUIRE(ok200 == kConns);

  Pump(loop, 10, 2);
  REQUIRE(server.connection_count() == 0);
}

TEST_CASE("TcpListener drains a backlog of pending connections under LT",
          "[net][epoll][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  std::vector<int> accepted;
  Result<std::unique_ptr<TcpListener>> listener = TcpListener::Create(
      loop, "127.0.0.1", 0, [&accepted](int fd) { accepted.push_back(fd); });
  REQUIRE(listener.ok());
  const std::uint16_t port = listener.value()->port();
  REQUIRE(port != 0);

  // Queue several connections before the loop ever runs, so a single readable
  // notification must accept all of them (the level-triggered drain loop).
  constexpr int kConns = 16;
  std::vector<int> clients;
  for (int i = 0; i < kConns; ++i) {
    const int c = ConnectLoopback(port);
    REQUIRE(c >= 0);
    clients.push_back(c);
  }

  Pump(loop, 20, 2);
  REQUIRE(static_cast<int>(accepted.size()) == kConns);

  for (const int c : clients) {
    ::close(c);
  }
  for (const int fd : accepted) {
    ::close(fd);
  }
}

TEST_CASE("Accepted sockets are close-on-exec (no fd leak into helpers)",
          "[net][epoll][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  int accepted_fd = -1;
  Result<std::unique_ptr<TcpListener>> listener = TcpListener::Create(
      loop, "127.0.0.1", 0, [&accepted_fd](int fd) { accepted_fd = fd; });
  REQUIRE(listener.ok());

  const int c = ConnectLoopback(listener.value()->port());
  REQUIRE(c >= 0);
  Pump(loop, 20, 2);
  REQUIRE(accepted_fd >= 0);

  // The server fork()/exec()s helper subprocesses; a client socket without
  // FD_CLOEXEC would leak into them and keep the peer connection half-open.
  const int fd_flags = ::fcntl(accepted_fd, F_GETFD, 0);
  REQUIRE(fd_flags >= 0);
  REQUIRE((fd_flags & FD_CLOEXEC) != 0);

  ::close(accepted_fd);
  ::close(c);
}
