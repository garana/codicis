/**
 * @file test_app.cc
 * @brief End-to-end test: REST -> matching engine -> storage helper.
 */

#include "catch_amalgamated.hpp"

#include "codicis/app/options.h"
#include "codicis/app/server.h"
#include "codicis/config/config.h"
#include "codicis/event/event_loop.h"
#include "codicis/util/clock.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace codicis;

namespace {

/** @brief Connect a blocking loopback client to @p port. */
int ConnectLoopback(std::uint16_t port) {
  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
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

/** @brief Send a request (asking to close) and read the response to EOF. */
std::string RoundTrip(EventLoop& loop, std::uint16_t port,
                      const std::string& request) {
  const int c = ConnectLoopback(port);
  REQUIRE(c >= 0);
  REQUIRE(::send(c, request.data(), request.size(), 0) ==
          static_cast<ssize_t>(request.size()));
  const int flags = ::fcntl(c, F_GETFL, 0);
  ::fcntl(c, F_SETFL, flags | O_NONBLOCK);

  std::string resp;
  bool done = false;
  for (int i = 0; i < 500 && !done; ++i) {
    loop.run_once(5);
    char buf[4096];
    for (;;) {
      const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
      if (n > 0) {
        resp.append(buf, static_cast<std::size_t>(n));
      } else if (n == 0) {
        done = true;
        break;
      } else {
        break;
      }
    }
  }
  ::close(c);
  return resp;
}

/** @brief Build a POST request with a form body. */
std::string Post(const std::string& path, const std::string& body) {
  return "POST " + path + " HTTP/1.1\r\nHost: x\r\nContent-Length: " +
         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

}  // namespace

TEST_CASE("REST submit matches and reports end-to-end", "[app]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  // Configure an ephemeral port and the freshly built storage helper.
  const OptionRegistry reg = BuildOptionRegistry();
  const std::string helper_flag =
      std::string("--storage.helper_cmd=") + CODICIS_STORAGE_HELPER_PATH;
  std::vector<const char*> argv = {"codicis", "--net.http_port=0",
                                   helper_flag.c_str()};
  Result<Config> cfg =
      Config::load(reg, static_cast<int>(argv.size()), argv.data());
  REQUIRE(cfg.ok());

  AppServer server(loop, cfg.value());
  REQUIRE(server.start().ok());
  const std::uint16_t port = server.http_port();
  REQUIRE(port != 0);

  SECTION("health") {
    const std::string resp =
        RoundTrip(loop, port,
                  "GET /health HTTP/1.1\r\nConnection: close\r\n\r\n");
    REQUIRE(resp.rfind("HTTP/1.1 200", 0) == 0);
    REQUIRE(resp.find("ok") != std::string::npos);
  }

  SECTION("crossing orders trade") {
    // Resting sell.
    std::string r1 =
        RoundTrip(loop, port, Post("/orders", "side=sell&type=limit&"
                                              "price=100&qty=10"));
    REQUIRE(r1.find("\"accepted\":true") != std::string::npos);

    // The book shows the resting ask.
    std::string rb =
        RoundTrip(loop, port,
                  "GET /book HTTP/1.1\r\nConnection: close\r\n\r\n");
    REQUIRE(rb.find("\"ask\":100") != std::string::npos);

    // Aggressive buy crosses and trades.
    std::string r2 =
        RoundTrip(loop, port, Post("/orders", "side=buy&type=limit&"
                                              "price=100&qty=10"));
    REQUIRE(r2.find("\"filled\":10") != std::string::npos);
    REQUIRE(r2.find("\"maker\":1") != std::string::npos);
  }

  SECTION("bad request is rejected") {
    std::string resp = RoundTrip(loop, port, Post("/orders", "side=buy"));
    REQUIRE(resp.rfind("HTTP/1.1 400", 0) == 0);
  }
}
