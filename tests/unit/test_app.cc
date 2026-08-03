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

/**
 * @brief Build a POST with extra header lines (each "Name: value\r\n").
 * @param path    The request target.
 * @param headers Additional header lines, concatenated verbatim.
 * @param body    The form body.
 */
std::string PostH(const std::string& path, const std::string& headers,
                  const std::string& body) {
  return "POST " + path + " HTTP/1.1\r\nHost: x\r\n" + headers +
         "Content-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

/** @brief A valid v4 UUID used as a test owner identity. */
const std::string kUserA = "11111111-1111-4111-8111-111111111111";
const std::string kUserB = "22222222-2222-4222-8222-222222222222";

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
    // Resting sell (anonymous: auth is disabled in this test).
    std::string r1 =
        RoundTrip(loop, port, Post("/orders", "symbol=BTC&side=sell&type=limit&"
                                              "price=100&qty=10"));
    REQUIRE(r1.find("\"accepted\":true") != std::string::npos);

    // The book shows the resting ask.
    std::string rb =
        RoundTrip(loop, port,
                  "GET /book?symbol=BTC HTTP/1.1\r\nConnection: close\r\n\r\n");
    REQUIRE(rb.find("\"ask\":100") != std::string::npos);

    // Aggressive buy crosses and trades.
    std::string r2 =
        RoundTrip(loop, port, Post("/orders", "symbol=BTC&side=buy&type=limit&"
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
      WsSubmit(loop, c, "symbol=BTC&side=sell&type=limit&price=100&qty=10");
  REQUIRE(r1.find("\"accepted\":true") != std::string::npos);

  const std::string r2 =
      WsSubmit(loop, c, "symbol=BTC&side=buy&type=limit&price=100&qty=10");
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
  REQUIRE(WsSubmit(loop, sub, "action=subscribe&symbol=BTC")
              .find("\"subscribed\":true") != std::string::npos);

  const int ord = WsConnect(loop, wsport);

  // A resting sell over the order connection publishes a book update.
  WsSend(loop, ord, "symbol=BTC&side=sell&type=limit&price=100&qty=10");
  REQUIRE(WsRecvFrame(loop, ord).find("\"accepted\":true") != std::string::npos);
  const std::string md1 = WsRecvFrame(loop, sub);
  REQUIRE(md1.find("\"type\":\"md\"") != std::string::npos);
  REQUIRE(md1.find("\"ask\":100") != std::string::npos);

  // A crossing buy publishes the resulting trade.
  WsSend(loop, ord, "symbol=BTC&side=buy&type=limit&price=100&qty=4");
  REQUIRE(WsRecvFrame(loop, ord).find("\"filled\":4") != std::string::npos);
  const std::string md2 = WsRecvFrame(loop, sub);
  REQUIRE(md2.find("\"trades\":[{\"price\":100,\"qty\":4}]") !=
          std::string::npos);

  ::close(sub);
  ::close(ord);
}

namespace {

/** @brief Common order body used by the auth tests. */
const std::string kOrderBody = "symbol=BTC&side=buy&type=limit&price=100&qty=5";

/** @brief The storage-helper CLI flag shared by every server under test. */
std::string StorageFlag() {
  return std::string("--storage.helper_cmd=") + CODICIS_STORAGE_HELPER_PATH;
}

}  // namespace

TEST_CASE("HTTP header auth supplies the owner identity", "[app][auth]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  const OptionRegistry reg = BuildOptionRegistry();
  const std::string storage = StorageFlag();
  std::vector<const char*> argv = {"codicis",     "--net.http_port=0",
                                   "--net.ws_port=0", storage.c_str(),
                                   "--auth.header.enabled=true"};
  Result<Config> cfg =
      Config::load(reg, static_cast<int>(argv.size()), argv.data());
  REQUIRE(cfg.ok());
  AppServer server(loop, cfg.value());
  REQUIRE(server.start().ok());
  const std::uint16_t port = server.http_port();

  SECTION("a valid identity header is accepted") {
    const std::string r = RoundTrip(
        loop, port, PostH("/orders", "X-User-Id: " + kUserA + "\r\n", kOrderBody));
    REQUIRE(r.rfind("HTTP/1.1 200", 0) == 0);
    REQUIRE(r.find("\"accepted\":true") != std::string::npos);
  }
  SECTION("a missing identity header is rejected 401") {
    const std::string r = RoundTrip(loop, port, Post("/orders", kOrderBody));
    REQUIRE(r.rfind("HTTP/1.1 401", 0) == 0);
  }
  SECTION("an invalid identity header is rejected 401") {
    const std::string r = RoundTrip(
        loop, port, PostH("/orders", "X-User-Id: not-a-uuid\r\n", kOrderBody));
    REQUIRE(r.rfind("HTTP/1.1 401", 0) == 0);
  }
}

TEST_CASE("HTTP helper auth validates a credential", "[app][auth]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  const OptionRegistry reg = BuildOptionRegistry();
  const std::string storage = StorageFlag();
  const std::string auth_cmd =
      std::string("--auth.helper.cmd=") + CODICIS_AUTH_HELPER_PATH;
  std::vector<const char*> argv = {
      "codicis",         "--net.http_port=0", "--net.ws_port=0",
      storage.c_str(),   "--auth.helper.enabled=true", auth_cmd.c_str()};
  Result<Config> cfg =
      Config::load(reg, static_cast<int>(argv.size()), argv.data());
  REQUIRE(cfg.ok());
  AppServer server(loop, cfg.value());
  REQUIRE(server.start().ok());
  const std::uint16_t port = server.http_port();

  SECTION("a valid credential is accepted") {
    const std::string r = RoundTrip(
        loop, port,
        PostH("/orders", "Authorization: " + kUserA + ":good\r\n", kOrderBody));
    REQUIRE(r.rfind("HTTP/1.1 200", 0) == 0);
    REQUIRE(r.find("\"accepted\":true") != std::string::npos);
  }
  SECTION("a bad credential is rejected 403") {
    const std::string r = RoundTrip(
        loop, port,
        PostH("/orders", "Authorization: " + kUserA + ":bad\r\n", kOrderBody));
    REQUIRE(r.rfind("HTTP/1.1 403", 0) == 0);
  }
  SECTION("a missing credential is rejected 401") {
    const std::string r = RoundTrip(loop, port, Post("/orders", kOrderBody));
    REQUIRE(r.rfind("HTTP/1.1 401", 0) == 0);
  }
}

TEST_CASE("HTTP header + helper auth must agree", "[app][auth]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  const OptionRegistry reg = BuildOptionRegistry();
  const std::string storage = StorageFlag();
  const std::string auth_cmd =
      std::string("--auth.helper.cmd=") + CODICIS_AUTH_HELPER_PATH;
  std::vector<const char*> argv = {"codicis",
                                   "--net.http_port=0",
                                   "--net.ws_port=0",
                                   storage.c_str(),
                                   "--auth.header.enabled=true",
                                   "--auth.helper.enabled=true",
                                   auth_cmd.c_str()};
  Result<Config> cfg =
      Config::load(reg, static_cast<int>(argv.size()), argv.data());
  REQUIRE(cfg.ok());
  AppServer server(loop, cfg.value());
  REQUIRE(server.start().ok());
  const std::uint16_t port = server.http_port();

  SECTION("matching header and credential are accepted") {
    const std::string r =
        RoundTrip(loop, port,
                  PostH("/orders",
                        "X-User-Id: " + kUserA + "\r\nAuthorization: " + kUserA +
                            ":good\r\n",
                        kOrderBody));
    REQUIRE(r.rfind("HTTP/1.1 200", 0) == 0);
  }
  SECTION("a mismatched header vs credential is rejected 403") {
    const std::string r =
        RoundTrip(loop, port,
                  PostH("/orders",
                        "X-User-Id: " + kUserA + "\r\nAuthorization: " + kUserB +
                            ":good\r\n",
                        kOrderBody));
    REQUIRE(r.rfind("HTTP/1.1 403", 0) == 0);
  }
}

TEST_CASE("REST session auction crosses queued orders", "[app][auction]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  const OptionRegistry reg = BuildOptionRegistry();
  const std::string storage = StorageFlag();
  std::vector<const char*> argv = {"codicis", "--net.http_port=0",
                                   "--net.ws_port=0", storage.c_str()};
  Result<Config> cfg =
      Config::load(reg, static_cast<int>(argv.size()), argv.data());
  REQUIRE(cfg.ok());
  AppServer server(loop, cfg.value());
  REQUIRE(server.start().ok());
  const std::uint16_t port = server.http_port();

  // Queue a limit-on-open buy and sell at the same price -- they must NOT
  // trade continuously, only in the auction.
  const std::string b = RoundTrip(
      loop, port,
      Post("/orders",
           "symbol=BTC&side=buy&type=limit&price=100&qty=10&auction=loo"));
  REQUIRE(b.find("\"accepted\":true") != std::string::npos);
  REQUIRE(b.find("\"rested\":false") != std::string::npos);  // queued, not resting

  const std::string s = RoundTrip(
      loop, port,
      Post("/orders",
           "symbol=BTC&side=sell&type=limit&price=100&qty=10&auction=loo"));
  REQUIRE(s.find("\"accepted\":true") != std::string::npos);

  // No continuous trade happened; the book top is empty.
  const std::string book = RoundTrip(
      loop, port,
      "GET /book?symbol=BTC HTTP/1.1\r\nConnection: close\r\n\r\n");
  REQUIRE(book.find("\"bid\":null") != std::string::npos);

  // Running the opening auction crosses them at the uniform price 100.
  const std::string a =
      RoundTrip(loop, port, Post("/auction", "symbol=BTC&phase=open"));
  REQUIRE(a.rfind("HTTP/1.1 200", 0) == 0);
  REQUIRE(a.find("\"phase\":\"open\"") != std::string::npos);
  REQUIRE(a.find("\"price\":100,\"qty\":10") != std::string::npos);

  // A bad phase is rejected.
  const std::string bad =
      RoundTrip(loop, port, Post("/auction", "symbol=BTC&phase=lunch"));
  REQUIRE(bad.rfind("HTTP/1.1 400", 0) == 0);
}

TEST_CASE("Deep levels are pulled back when an aggressor reaches them",
          "[app][window]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  const OptionRegistry reg = BuildOptionRegistry();
  const std::string storage = StorageFlag();
  std::vector<const char*> argv = {"codicis", "--net.http_port=0",
                                   "--net.ws_port=0", storage.c_str(),
                                   "--book.mem_levels=2"};
  Result<Config> cfg =
      Config::load(reg, static_cast<int>(argv.size()), argv.data());
  REQUIRE(cfg.ok());
  AppServer server(loop, cfg.value());
  REQUIRE(server.start().ok());
  const std::uint16_t port = server.http_port();

  // Two resident ask levels fill the window; the third rests deep.
  RoundTrip(loop, port,
            Post("/orders", "symbol=BTC&side=sell&type=limit&price=100&qty=5"));
  RoundTrip(loop, port,
            Post("/orders", "symbol=BTC&side=sell&type=limit&price=101&qty=5"));
  const std::string deep = RoundTrip(
      loop, port,
      Post("/orders", "symbol=BTC&side=sell&type=limit&price=102&qty=5"));
  REQUIRE(deep.find("\"accepted\":true") != std::string::npos);
  REQUIRE(deep.find("\"rested\":false") != std::string::npos);  // rests deep

  // The book shows only the resident top (best ask 100).
  const std::string book = RoundTrip(
      loop, port, "GET /book?symbol=BTC HTTP/1.1\r\nConnection: close\r\n\r\n");
  REQUIRE(book.find("\"ask\":100") != std::string::npos);

  // A buy that crosses to 102 must pull the deep level back and fill all three.
  const std::string buy = RoundTrip(
      loop, port,
      Post("/orders", "symbol=BTC&side=buy&type=limit&price=102&qty=15"));
  REQUIRE(buy.find("\"filled\":15") != std::string::npos);
}
