/**
 * @file http_server.cc
 * @brief Implementation of HttpServer and its per-connection state machine.
 */

#include "codicis/net/http_server.h"

#include <unistd.h>

#include <cerrno>
#include <functional>
#include <string>
#include <utility>

#include "codicis/net/http_parser.h"
#include "codicis/util/buffer.h"

namespace codicis {

/**
 * @brief One client connection: read -> parse -> route -> write, non-blocking.
 *
 * Internal to the net subsystem. The connection never destroys itself
 * synchronously; when it decides to close it invokes an owner callback that
 * unregisters the descriptor and defers destruction to a safe point.
 */
class HttpConnection final : public IoHandler {
 public:
  /**
   * @brief Construct a connection over an accepted descriptor.
   * @param loop     The event loop (must outlive this).
   * @param fd       The accepted, non-blocking descriptor (owned here).
   * @param server   The owning server (for delivering async responses).
   * @param router   The route table (must outlive this).
   * @param id       A unique connection id (never reused).
   * @param on_close Owner callback invoked once when the connection closes.
   */
  HttpConnection(EventLoop& loop, int fd, HttpServer& server,
                 const HttpRouter& router, std::uint64_t id,
                 std::function<void(int)> on_close)
      : loop_(loop),
        fd_(fd),
        server_(server),
        router_(router),
        id_(id),
        on_close_(std::move(on_close)) {}

  ~HttpConnection() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  /** @return This connection's unique id. */
  std::uint64_t id() const { return id_; }

  /**
   * @brief Deliver a (possibly deferred) response and resume processing.
   * @param resp       The response to send.
   * @param keep_alive Whether to keep the connection open afterwards.
   */
  void complete_response(HttpResponse resp, bool keep_alive) {
    if (closed_) {
      return;
    }
    out_.append(resp.serialize(keep_alive));
    awaiting_response_ = false;
    if (!keep_alive) {
      closing_ = true;
    }
    if (in_dispatch_) {
      return;  // synchronous handler: process_input's loop will continue
    }
    process_input();  // async completion: parse the next request and flush
  }

  void on_io_ready(int /*fd*/, IoEvents events) override {
    if (HasEvent(events, IoEvents::kReadable)) {
      on_readable();
      if (closed_) {
        return;
      }
    }
    if (HasEvent(events, IoEvents::kWritable)) {
      on_writable();
      if (closed_) {
        return;
      }
    }
    if (HasEvent(events, IoEvents::kError)) {
      close();
    }
  }

 private:
  /** @brief Drain the socket into the input buffer, then parse. */
  void on_readable() {
    for (;;) {
      std::uint8_t* dst = in_.reserve(kReadChunk);
      const ssize_t n = ::read(fd_, dst, kReadChunk);
      if (n > 0) {
        in_.commit(static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) {
        peer_closed_ = true;
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      close();
      return;
    }
    process_input();
  }

  /**
   * @brief Parse and dispatch buffered requests until one goes async.
   *
   * At most one request is outstanding at a time: while a response is pending
   * (@ref awaiting_response_), no further request is parsed, preserving HTTP/1.1
   * response ordering. A synchronous handler completes within @ref
   * dispatch_request; an async handler leaves the loop and resumes via
   * @ref complete_response.
   */
  void process_input() {
    while (!awaiting_response_) {
      const HttpRequestParser::Progress p = parser_.parse(in_);
      if (p == HttpRequestParser::Progress::kComplete) {
        dispatch_request();
        if (awaiting_response_) {
          return;  // async handler in flight; wait for its response
        }
        if (closing_) {
          break;
        }
        continue;
      }
      if (p == HttpRequestParser::Progress::kIncomplete) {
        break;
      }
      // kError: emit a minimal 400 and close.
      HttpResponse resp;
      resp.set_status(400);
      resp.set_header("Content-Type", "text/plain");
      resp.body = "Bad Request";
      out_.append(resp.serialize(/*keep_alive=*/false));
      closing_ = true;
      break;
    }

    if (!out_.empty()) {
      want_write(true);
      on_writable();
      if (closed_) {
        return;
      }
    }
    // Do not close while an async response is still pending.
    if (out_.empty() && !awaiting_response_ && (closing_ || peer_closed_)) {
      close();
    }
  }

  /**
   * @brief Route the current request; the response may be delivered later.
   *
   * The responder is routed through the server keyed by (fd, connection id) so
   * it stays safe if the connection is torn down before the response arrives.
   */
  void dispatch_request() {
    const HttpRequest& req = parser_.request();
    keep_alive_current_ = req.keep_alive;
    awaiting_response_ = true;
    in_dispatch_ = true;

    HttpServer* server = &server_;
    const int fd = fd_;
    const std::uint64_t id = id_;
    const bool keep_alive = keep_alive_current_;
    HttpResponder responder = [server, fd, id, keep_alive](HttpResponse resp) {
      server->deliver_response(fd, id, std::move(resp), keep_alive);
    };
    router_.dispatch(req, std::move(responder));
    parser_.reset();
    in_dispatch_ = false;
  }

  /** @brief Flush the output buffer; close if done and closing. */
  void on_writable() {
    while (!out_.empty()) {
      const ssize_t n = ::write(fd_, out_.data(), out_.size());
      if (n > 0) {
        out_.consume(static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;  // Socket full; wait for the next writable event.
      }
      close();
      return;
    }
    if (out_.empty()) {
      want_write(false);
      if (closing_) {
        close();
      }
    }
  }

  /** @brief Enable or disable interest in writability (read stays on). */
  void want_write(bool on) {
    if (on == write_enabled_) {
      return;
    }
    write_enabled_ = on;
    const IoInterest interest =
        on ? (IoInterest::kRead | IoInterest::kWrite) : IoInterest::kRead;
    loop_.modify(fd_, interest);
  }

  /** @brief Notify the owner to tear down this connection (once). */
  void close() {
    if (closed_) {
      return;
    }
    closed_ = true;
    on_close_(fd_);
  }

  static constexpr std::size_t kReadChunk = 16 * 1024;

  EventLoop& loop_;
  int fd_;
  HttpServer& server_;
  const HttpRouter& router_;
  std::uint64_t id_;
  std::function<void(int)> on_close_;

  Buffer in_;
  Buffer out_;
  HttpRequestParser parser_;
  bool write_enabled_ = false;
  bool closing_ = false;
  bool peer_closed_ = false;
  bool closed_ = false;
  bool awaiting_response_ = false;  /**< A response is pending (sync or async). */
  bool in_dispatch_ = false;        /**< Inside dispatch_request (sync detect). */
  bool keep_alive_current_ = true;  /**< Keep-alive of the in-flight request. */
};

// ---- HttpServer ------------------------------------------------------------

HttpServer::HttpServer(EventLoop& loop, const HttpRouter& router)
    : loop_(loop), router_(router) {}

HttpServer::~HttpServer() = default;

Status HttpServer::listen(const std::string& addr, std::uint16_t port) {
  Result<std::unique_ptr<TcpListener>> lr = TcpListener::Create(
      loop_, addr, port, [this](int fd) { on_accept(fd); });
  if (!lr.ok()) {
    return lr.error();
  }
  listener_ = std::move(lr.value());
  return Status::Ok();
}

std::uint16_t HttpServer::port() const {
  return listener_ ? listener_->port() : 0;
}

void HttpServer::on_accept(int fd) {
  const std::uint64_t id = next_conn_id_++;
  auto conn = std::make_unique<HttpConnection>(
      loop_, fd, *this, router_, id,
      [this](int cfd) { close_connection(cfd); });
  if (Status s = loop_.add(fd, IoInterest::kRead, conn.get()); !s.ok()) {
    return;  // conn destructs, closing fd.
  }
  conns_.emplace(fd, std::move(conn));
}

void HttpServer::deliver_response(int fd, std::uint64_t id, HttpResponse resp,
                                  bool keep_alive) {
  const auto it = conns_.find(fd);
  if (it == conns_.end() || it->second->id() != id) {
    return;  // connection gone, or fd reused by a newer connection
  }
  it->second->complete_response(std::move(resp), keep_alive);
}

void HttpServer::close_connection(int fd) {
  loop_.remove(fd);
  // Defer erase so we never destroy a connection from within its own callback.
  loop_.defer([this, fd]() { conns_.erase(fd); });
}

}  // namespace codicis
