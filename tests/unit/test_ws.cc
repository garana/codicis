/**
 * @file test_ws.cc
 * @brief Tests for the WebSocket codec, handshake, and echo server.
 */

#include "catch_amalgamated.hpp"

#include "codicis/event/event_loop.h"
#include "codicis/net/websocket.h"
#include "codicis/net/ws_frame.h"
#include "codicis/net/ws_handshake.h"
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
#include <string>

using namespace codicis;

namespace {

const std::array<std::uint8_t, 4> kMask = {0x37, 0xfa, 0x21, 0x3d};

/** @brief Feed bytes to a buffer. */
Buffer MakeBuffer(std::string_view s) {
  Buffer b;
  b.append(s);
  return b;
}

}  // namespace

TEST_CASE("Accept key matches the RFC 6455 example", "[ws][handshake]") {
  REQUIRE(WsComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("Decodes the RFC 6455 masked 'Hello' frame", "[ws][frame]") {
  const std::uint8_t bytes[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                0x7f, 0x9f, 0x4d, 0x51, 0x58};
  Buffer b;
  b.append(std::string_view(reinterpret_cast<const char*>(bytes),
                            sizeof(bytes)));
  WsFrame f;
  std::string err;
  REQUIRE(DecodeWsFrame(b, &f, &err) == WsDecode::kComplete);
  REQUIRE(f.fin);
  REQUIRE(f.opcode == WsOpcode::kText);
  REQUIRE(f.payload == "Hello");
  REQUIRE(b.empty());
}

TEST_CASE("Frame encode/decode round-trips", "[ws][frame]") {
  SECTION("unmasked short text") {
    const std::string wire = EncodeWsFrame(WsOpcode::kText, "Hi");
    Buffer b = MakeBuffer(wire);
    WsFrame f;
    std::string err;
    REQUIRE(DecodeWsFrame(b, &f, &err) == WsDecode::kComplete);
    REQUIRE(f.payload == "Hi");
    REQUIRE(f.opcode == WsOpcode::kText);
  }
  SECTION("masked payload is unmasked on decode") {
    const std::string wire =
        EncodeWsFrame(WsOpcode::kBinary, "abcd", true, kMask);
    Buffer b = MakeBuffer(wire);
    WsFrame f;
    std::string err;
    REQUIRE(DecodeWsFrame(b, &f, &err) == WsDecode::kComplete);
    REQUIRE(f.opcode == WsOpcode::kBinary);
    REQUIRE(f.payload == "abcd");
  }
  SECTION("extended 16-bit length") {
    const std::string big(200, 'z');
    const std::string wire = EncodeWsFrame(WsOpcode::kText, big);
    Buffer b = MakeBuffer(wire);
    WsFrame f;
    std::string err;
    REQUIRE(DecodeWsFrame(b, &f, &err) == WsDecode::kComplete);
    REQUIRE(f.payload == big);
  }
}

TEST_CASE("Incomplete frame is not consumed", "[ws][frame]") {
  const std::string wire = EncodeWsFrame(WsOpcode::kText, "hello");
  Buffer b = MakeBuffer(wire.substr(0, wire.size() - 2));
  WsFrame f;
  std::string err;
  REQUIRE(DecodeWsFrame(b, &f, &err) == WsDecode::kIncomplete);
  REQUIRE(b.size() == wire.size() - 2);  // nothing consumed
}

// ---- Echo server integration ----------------------------------------------

namespace {

/** @brief Set a descriptor non-blocking. */
void SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/** @brief Connect a non-blocking loopback client to @p port. */
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
  SetNonBlocking(c);
  return c;
}

/** @brief Send all bytes, pumping the loop if the socket is momentarily full. */
void SendAll(EventLoop& loop, int c, std::string_view data) {
  std::size_t sent = 0;
  for (int i = 0; i < 200 && sent < data.size(); ++i) {
    const ssize_t n = ::send(c, data.data() + sent, data.size() - sent, 0);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
    } else {
      loop.run_once(5);
    }
  }
  REQUIRE(sent == data.size());
}

/** @brief Pump the loop, appending any received bytes to @p acc. */
void PumpAndDrain(EventLoop& loop, int c, std::string& acc) {
  loop.run_once(5);
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
    if (n > 0) {
      acc.append(buf, static_cast<std::size_t>(n));
    } else {
      break;
    }
  }
}

}  // namespace

TEST_CASE("WsServer completes the handshake and echoes messages",
          "[ws][server]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  // Echo callback: reply with the same payload and kind.
  WsServer server(loop, [](WsConnection& conn, bool is_binary,
                           std::string_view payload) {
    if (is_binary) {
      conn.send_binary(payload);
    } else {
      conn.send_text(payload);
    }
  });
  REQUIRE(server.listen("127.0.0.1", 0).ok());

  const int c = ConnectLoopback(server.port());
  REQUIRE(c >= 0);

  // Opening handshake.
  SendAll(loop, c,
          "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
          "Connection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 13\r\n\r\n");

  std::string resp;
  for (int i = 0; i < 100 && resp.find("\r\n\r\n") == std::string::npos; ++i) {
    PumpAndDrain(loop, c, resp);
  }
  REQUIRE(resp.rfind("HTTP/1.1 101", 0) == 0);
  REQUIRE(resp.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

  SECTION("single text frame is echoed") {
    SendAll(loop, c, EncodeWsFrame(WsOpcode::kText, "hello", true, kMask));
    std::string acc;
    WsFrame f;
    std::string err;
    Buffer rb;
    for (int i = 0; i < 100; ++i) {
      PumpAndDrain(loop, c, acc);
      rb.clear();
      rb.append(acc);
      if (DecodeWsFrame(rb, &f, &err) == WsDecode::kComplete) {
        break;
      }
    }
    REQUIRE(f.opcode == WsOpcode::kText);
    REQUIRE(f.payload == "hello");
  }

  SECTION("fragmented message is reassembled and echoed") {
    SendAll(loop, c, EncodeWsFrame(WsOpcode::kText, "foo", false, kMask));
    SendAll(loop, c,
            EncodeWsFrame(WsOpcode::kContinuation, "bar", true, kMask));
    std::string acc;
    WsFrame f;
    std::string err;
    Buffer rb;
    for (int i = 0; i < 100; ++i) {
      PumpAndDrain(loop, c, acc);
      rb.clear();
      rb.append(acc);
      if (DecodeWsFrame(rb, &f, &err) == WsDecode::kComplete) {
        break;
      }
    }
    REQUIRE(f.payload == "foobar");
  }

  ::close(c);
}
