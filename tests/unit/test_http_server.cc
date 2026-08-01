/**
 * @file test_http_server.cc
 * @brief Integration tests: drive HttpServer over a real loopback socket.
 */

#include "catch_amalgamated.hpp"

#include "codicis/event/event_loop.h"
#include "codicis/net/http_server.h"
#include "codicis/net/router.h"
#include "codicis/util/clock.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

using namespace codicis;

namespace {

/**
 * @brief Open a blocking loopback client connected to @p port.
 * @param port The server port.
 * @return The connected client descriptor, or -1 on failure.
 */
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

/**
 * @brief Send @p request, pump the loop, and read the full response to EOF.
 *
 * The request should ask the server to close the connection so the read side
 * terminates deterministically at EOF.
 * @param loop    The event loop running the server.
 * @param port    The server port.
 * @param request The raw request bytes to send.
 * @return The raw response bytes received.
 */
std::string RoundTripToClose(EventLoop& loop, std::uint16_t port,
                             const std::string& request) {
  const int c = ConnectLoopback(port);
  REQUIRE(c >= 0);
  REQUIRE(::send(c, request.data(), request.size(), 0) ==
          static_cast<ssize_t>(request.size()));

  const int flags = ::fcntl(c, F_GETFL, 0);
  ::fcntl(c, F_SETFL, flags | O_NONBLOCK);

  std::string response;
  bool done = false;
  for (int i = 0; i < 500 && !done; ++i) {
    loop.run_once(5);
    char buf[4096];
    for (;;) {
      const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
      if (n > 0) {
        response.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) {
        done = true;  // server closed
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;  // nothing more for now; pump again
      }
      done = true;
      break;
    }
  }
  ::close(c);
  return response;
}

/** @brief Build a router with /health and POST /echo. */
HttpRouter MakeRouter() {
  HttpRouter router;
  router.add("GET", "/health", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200);
    resp.set_header("Content-Type", "text/plain");
    resp.body = "ok";
  });
  router.add("POST", "/echo", [](const HttpRequest& req, HttpResponse& resp) {
    resp.set_status(200);
    resp.set_header("Content-Type", "application/octet-stream");
    resp.body = req.body;
  });
  return router;
}

}  // namespace

TEST_CASE("GET /health returns 200 ok", "[http][server]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter();
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const std::string resp = RoundTripToClose(
      loop, server.port(),
      "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  REQUIRE(resp.rfind("HTTP/1.1 200", 0) == 0);
  REQUIRE(resp.find("ok") != std::string::npos);
}

TEST_CASE("POST /echo returns the request body", "[http][server]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter();
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const std::string resp = RoundTripToClose(
      loop, server.port(),
      "POST /echo HTTP/1.1\r\nContent-Length: 5\r\nConnection: close\r\n"
      "\r\nhello");
  REQUIRE(resp.rfind("HTTP/1.1 200", 0) == 0);
  REQUIRE(resp.find("\r\n\r\nhello") != std::string::npos);
}

TEST_CASE("Unknown path returns 404", "[http][server]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter();
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const std::string resp = RoundTripToClose(
      loop, server.port(),
      "GET /nope HTTP/1.1\r\nConnection: close\r\n\r\n");
  REQUIRE(resp.rfind("HTTP/1.1 404", 0) == 0);
}

TEST_CASE("Wrong method returns 405", "[http][server]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter();
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const std::string resp = RoundTripToClose(
      loop, server.port(),
      "POST /health HTTP/1.1\r\nContent-Length: 0\r\nConnection: close\r\n"
      "\r\n");
  REQUIRE(resp.rfind("HTTP/1.1 405", 0) == 0);
}

TEST_CASE("Keep-alive serves pipelined requests on one connection",
          "[http][server]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  HttpRouter router = MakeRouter();
  HttpServer server(loop, router);
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  // Two keep-alive requests then one that closes; expect three 200s.
  const std::string resp = RoundTripToClose(
      loop, server.port(),
      "GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
      "GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
      "GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = resp.find("HTTP/1.1 200", pos)) != std::string::npos) {
    ++count;
    pos += 1;
  }
  REQUIRE(count == 3);
}
