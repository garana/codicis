/**
 * @file test_app.cc
 * @brief End-to-end test: REST -> matching engine -> storage helper.
 */

#include "catch_amalgamated.hpp"

#include "codicis/app/options.h"
#include "codicis/app/server.h"
#include "codicis/config/config.h"
#include "codicis/event/event_loop.h"
#include "codicis/net/ws_frame.h"
#include "codicis/util/buffer.h"
#include "codicis/util/clock.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
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
                                   "--net.ws_port=0", helper_flag.c_str()};
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

namespace {

const std::array<std::uint8_t, 4> kWsMask = {0x11, 0x22, 0x33, 0x44};

/** @brief Open a WebSocket connection to @p port and complete the handshake. */
int WsConnect(EventLoop& loop, std::uint16_t port) {
  const int c = ConnectLoopback(port);
  REQUIRE(c >= 0);
  const std::string handshake =
      "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  REQUIRE(::send(c, handshake.data(), handshake.size(), 0) ==
          static_cast<ssize_t>(handshake.size()));
  const int flags = ::fcntl(c, F_GETFL, 0);
  ::fcntl(c, F_SETFL, flags | O_NONBLOCK);

  std::string resp;
  for (int i = 0; i < 100 && resp.find("\r\n\r\n") == std::string::npos; ++i) {
    loop.run_once(5);
    char buf[2048];
    ssize_t n = 0;
    while ((n = ::recv(c, buf, sizeof(buf), 0)) > 0) {
      resp.append(buf, static_cast<std::size_t>(n));
    }
  }
  REQUIRE(resp.rfind("HTTP/1.1 101", 0) == 0);
  return c;
}

/** @brief Send a masked text frame over the WS connection (no wait). */
void WsSend(EventLoop& loop, int c, const std::string& body) {
  const std::string frame = EncodeWsFrame(WsOpcode::kText, body, true, kWsMask);
  std::size_t sent = 0;
  for (int i = 0; i < 100 && sent < frame.size(); ++i) {
    const ssize_t n = ::send(c, frame.data() + sent, frame.size() - sent, 0);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
    } else {
      loop.run_once(5);
    }
  }
}

/** @brief Pump the loop and return the next decoded text frame payload. */
std::string WsRecvFrame(EventLoop& loop, int c) {
  Buffer rb;
  WsFrame f;
  std::string err;
  for (int i = 0; i < 200; ++i) {
    loop.run_once(5);
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::recv(c, buf, sizeof(buf), 0)) > 0) {
      rb.append(buf, static_cast<std::size_t>(n));
    }
    if (DecodeWsFrame(rb, &f, &err) == WsDecode::kComplete) {
      return f.payload;
    }
  }
  return "";
}

/** @brief Send an order/request and return the single reply payload. */
std::string WsSubmit(EventLoop& loop, int c, const std::string& body) {
  WsSend(loop, c, body);
  return WsRecvFrame(loop, c);
}

}  // namespace

TEST_CASE("WebSocket order submission matches end-to-end", "[app][ws]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  const OptionRegistry reg = BuildOptionRegistry();
  const std::string helper_flag =
      std::string("--storage.helper_cmd=") + CODICIS_STORAGE_HELPER_PATH;
  std::vector<const char*> argv = {"codicis", "--net.http_port=0",
                                   "--net.ws_port=0", helper_flag.c_str()};
  Result<Config> cfg =
      Config::load(reg, static_cast<int>(argv.size()), argv.data());
  REQUIRE(cfg.ok());

  AppServer server(loop, cfg.value());
  REQUIRE(server.start().ok());
  const std::uint16_t wsport = server.ws_port();
  REQUIRE(wsport != 0);

  const int c = WsConnect(loop, wsport);

  // Two orders on one WebSocket connection: a resting sell, then a crossing buy.
  const std::string r1 =
      WsSubmit(loop, c, "side=sell&type=limit&price=100&qty=10");
  REQUIRE(r1.find("\"accepted\":true") != std::string::npos);

  const std::string r2 =
      WsSubmit(loop, c, "side=buy&type=limit&price=100&qty=10");
  REQUIRE(r2.find("\"filled\":10") != std::string::npos);
  REQUIRE(r2.find("\"maker\":1") != std::string::npos);

  ::close(c);
}

TEST_CASE("WebSocket subscribers receive market-data updates", "[app][ws][md]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  const OptionRegistry reg = BuildOptionRegistry();
  const std::string helper_flag =
      std::string("--storage.helper_cmd=") + CODICIS_STORAGE_HELPER_PATH;
  std::vector<const char*> argv = {"codicis", "--net.http_port=0",
                                   "--net.ws_port=0", helper_flag.c_str()};
  Result<Config> cfg =
      Config::load(reg, static_cast<int>(argv.size()), argv.data());
  REQUIRE(cfg.ok());
  AppServer server(loop, cfg.value());
  REQUIRE(server.start().ok());
  const std::uint16_t wsport = server.ws_port();

  const int sub = WsConnect(loop, wsport);
  REQUIRE(WsSubmit(loop, sub, "action=subscribe")
              .find("\"subscribed\":true") != std::string::npos);

  const int ord = WsConnect(loop, wsport);

  // A resting sell over the order connection publishes a book update.
  WsSend(loop, ord, "side=sell&type=limit&price=100&qty=10");
  REQUIRE(WsRecvFrame(loop, ord).find("\"accepted\":true") != std::string::npos);
  const std::string md1 = WsRecvFrame(loop, sub);
  REQUIRE(md1.find("\"type\":\"md\"") != std::string::npos);
  REQUIRE(md1.find("\"ask\":100") != std::string::npos);

  // A crossing buy publishes the resulting trade in the market-data stream.
  WsSend(loop, ord, "side=buy&type=limit&price=100&qty=4");
  REQUIRE(WsRecvFrame(loop, ord).find("\"filled\":4") != std::string::npos);
  const std::string md2 = WsRecvFrame(loop, sub);
  REQUIRE(md2.find("\"trades\":[{\"price\":100,\"qty\":4}]") !=
          std::string::npos);

  ::close(sub);
  ::close(ord);
}
