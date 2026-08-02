#ifndef CODICIS_NET_WEBSOCKET_H
#define CODICIS_NET_WEBSOCKET_H

/**
 * @file websocket.h
 * @brief An event-loop-driven WebSocket server (RFC 6455).
 *
 * WsServer accepts TCP connections, performs the opening HTTP handshake, then
 * exchanges frames. Fragmented data messages are reassembled; control frames
 * (ping/pong/close) are handled automatically. Application code receives whole
 * messages via a callback and replies with @ref WsConnection::send_text /
 * @ref WsConnection::send_binary.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "codicis/event/event_loop.h"
#include "codicis/net/http_parser.h"
#include "codicis/net/listener.h"
#include "codicis/net/ws_frame.h"
#include "codicis/util/buffer.h"
#include "codicis/util/result.h"

namespace codicis {

class WsConnection;

/**
 * @brief Callback delivering a complete WebSocket message.
 * @param conn      The originating connection (use it to reply).
 * @param is_binary True for a binary message, false for text.
 * @param payload   The full (reassembled) message payload.
 */
using WsMessageFn = std::function<void(WsConnection& conn, bool is_binary,
                                       std::string_view payload)>;

/**
 * @brief One WebSocket connection: handshake, then framed messaging.
 */
class WsConnection final : public IoHandler {
 public:
  /**
   * @brief Construct over an accepted descriptor (owned here).
   * @param loop       The event loop (must outlive this).
   * @param fd         The accepted, non-blocking descriptor.
   * @param id         A unique connection id (never reused).
   * @param on_message Message callback.
   * @param on_close   Owner callback invoked once when the connection closes.
   */
  WsConnection(EventLoop& loop, int fd, std::uint64_t id,
               WsMessageFn on_message, std::function<void(int)> on_close);

  ~WsConnection() override;

  void on_io_ready(int fd, IoEvents events) override;

  /** @return This connection's descriptor. */
  int fd() const { return fd_; }

  /** @return This connection's unique id. */
  std::uint64_t id() const { return id_; }

  /**
   * @brief Send a text message (single unfragmented frame).
   * @param text The UTF-8 payload.
   */
  void send_text(std::string_view text);

  /**
   * @brief Send a binary message (single unfragmented frame).
   * @param data The payload bytes.
   */
  void send_binary(std::string_view data);

  /**
   * @brief Send a Close frame and begin closing.
   * @param code   The close status code (default 1000, normal closure).
   * @param reason An optional UTF-8 reason.
   */
  void send_close(std::uint16_t code = 1000, std::string_view reason = "");

  /** @return True once the opening handshake has completed. */
  bool is_open() const { return open_; }

 private:
  void on_readable();
  void on_writable();
  void do_handshake();
  void process_frames();
  void handle_frame(const WsFrame& frame);
  void queue(std::string bytes);
  void want_write(bool on);
  void close();

  EventLoop& loop_;
  int fd_;
  std::uint64_t id_;
  WsMessageFn on_message_;
  std::function<void(int)> on_close_;

  Buffer in_;
  Buffer out_;
  HttpRequestParser handshake_parser_;

  bool open_ = false;
  bool write_enabled_ = false;
  bool closing_ = false;
  bool closed_ = false;

  // Fragmentation reassembly for data messages.
  bool assembling_ = false;
  bool assembling_binary_ = false;
  std::string assembly_;
};

/**
 * @brief A WebSocket server bound to a TCP port.
 */
class WsServer {
 public:
  /**
   * @brief Construct a server.
   * @param loop       The event loop (must outlive the server).
   * @param on_message Callback invoked per complete message.
   */
  WsServer(EventLoop& loop, WsMessageFn on_message);

  ~WsServer();

  WsServer(const WsServer&) = delete;
  WsServer& operator=(const WsServer&) = delete;

  /**
   * @brief Begin listening.
   * @param addr Dotted IPv4 bind address.
   * @param port Bind port; 0 selects an ephemeral port.
   * @return Ok, or an Error on socket setup failure.
   */
  Status listen(const std::string& addr, std::uint16_t port);

  /** @return The bound port (valid after a successful @ref listen). */
  std::uint16_t port() const;

  /** @return The number of currently open connections. */
  std::size_t connection_count() const { return conns_.size(); }

  /**
   * @brief Send a text message to a connection, if it still exists.
   *
   * Looks up the connection by @p fd and verifies @p id (guarding against a
   * descriptor reused by a newer connection); drops the message if the
   * original connection is gone. Safe to call from a deferred callback.
   * @param fd   The connection's descriptor.
   * @param id   The connection's unique id.
   * @param text The message to send.
   */
  void deliver_text(int fd, std::uint64_t id, std::string_view text);

 private:
  void on_accept(int fd);
  void close_connection(int fd);

  EventLoop& loop_;
  WsMessageFn on_message_;
  std::unique_ptr<TcpListener> listener_;
  std::unordered_map<int, std::unique_ptr<WsConnection>> conns_;
  std::uint64_t next_conn_id_ = 1;
};

}  // namespace codicis

#endif  // CODICIS_NET_WEBSOCKET_H
