/**
 * @file websocket.cc
 * @brief Implementation of WsConnection and WsServer (see websocket.h).
 */

#include "codicis/net/websocket.h"

#include <unistd.h>

#include <cerrno>
#include <utility>

#include "codicis/net/ws_handshake.h"

namespace codicis {
namespace {

constexpr std::size_t kReadChunk = 16 * 1024;

}  // namespace

// ---- WsConnection ----------------------------------------------------------

WsConnection::WsConnection(EventLoop& loop, int fd, std::uint64_t id,
                           WsMessageFn on_message,
                           std::function<void(int)> on_close)
    : loop_(loop),
      fd_(fd),
      id_(id),
      on_message_(std::move(on_message)),
      on_close_(std::move(on_close)) {}

WsConnection::~WsConnection() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void WsConnection::on_io_ready(int /*fd*/, IoEvents events) {
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

void WsConnection::on_readable() {
  for (;;) {
    std::uint8_t* dst = in_.reserve(kReadChunk);
    const ssize_t n = ::read(fd_, dst, kReadChunk);
    if (n > 0) {
      in_.commit(static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      close();
      return;
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

  if (!open_) {
    do_handshake();
    if (closed_) {
      return;
    }
  }
  if (open_) {
    process_frames();
    if (closed_) {
      return;
    }
  }

  if (!out_.empty()) {
    want_write(true);
    on_writable();
  }
}

void WsConnection::do_handshake() {
  const HttpRequestParser::Progress p = handshake_parser_.parse(in_);
  if (p == HttpRequestParser::Progress::kIncomplete) {
    return;
  }
  if (p == HttpRequestParser::Progress::kError) {
    close();
    return;
  }
  std::string key;
  if (!WsIsUpgradeRequest(handshake_parser_.request(), &key)) {
    out_.append(
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n");
    closing_ = true;
    return;
  }
  out_.append(WsBuildHandshakeResponse(key));
  open_ = true;
}

void WsConnection::process_frames() {
  for (;;) {
    WsFrame frame;
    std::string err;
    const WsDecode d = DecodeWsFrame(in_, &frame, &err);
    if (d == WsDecode::kIncomplete) {
      return;
    }
    if (d == WsDecode::kError) {
      close();
      return;
    }
    handle_frame(frame);
    if (closed_ || closing_) {
      return;
    }
  }
}

void WsConnection::handle_frame(const WsFrame& frame) {
  switch (frame.opcode) {
    case WsOpcode::kPing:
      queue(EncodeWsFrame(WsOpcode::kPong, frame.payload));
      return;
    case WsOpcode::kPong:
      return;  // unsolicited pongs are ignored
    case WsOpcode::kClose:
      // Echo the close and begin closing.
      queue(EncodeWsFrame(WsOpcode::kClose, frame.payload));
      closing_ = true;
      return;
    case WsOpcode::kText:
    case WsOpcode::kBinary: {
      const bool is_binary = (frame.opcode == WsOpcode::kBinary);
      if (frame.fin) {
        on_message_(*this, is_binary, frame.payload);
      } else {
        assembling_ = true;
        assembling_binary_ = is_binary;
        assembly_ = frame.payload;
      }
      return;
    }
    case WsOpcode::kContinuation:
      if (!assembling_) {
        close();
        return;
      }
      assembly_ += frame.payload;
      if (frame.fin) {
        assembling_ = false;
        on_message_(*this, assembling_binary_, assembly_);
        assembly_.clear();
      }
      return;
  }
}

void WsConnection::send_text(std::string_view text) {
  queue(EncodeWsFrame(WsOpcode::kText, text));
  want_write(true);
  on_writable();
}

void WsConnection::send_binary(std::string_view data) {
  queue(EncodeWsFrame(WsOpcode::kBinary, data));
  want_write(true);
  on_writable();
}

void WsConnection::send_close(std::uint16_t code, std::string_view reason) {
  std::string payload;
  payload.push_back(static_cast<char>((code >> 8) & 0xFF));
  payload.push_back(static_cast<char>(code & 0xFF));
  payload.append(reason);
  queue(EncodeWsFrame(WsOpcode::kClose, payload));
  closing_ = true;
  want_write(true);
  on_writable();
}

void WsConnection::queue(std::string bytes) { out_.append(bytes); }

void WsConnection::on_writable() {
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
      break;
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

void WsConnection::want_write(bool on) {
  if (on == write_enabled_) {
    return;
  }
  write_enabled_ = on;
  const IoInterest interest =
      on ? (IoInterest::kRead | IoInterest::kWrite) : IoInterest::kRead;
  loop_.modify(fd_, interest);
}

void WsConnection::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  on_close_(fd_);
}

// ---- WsServer --------------------------------------------------------------

WsServer::WsServer(EventLoop& loop, WsMessageFn on_message)
    : loop_(loop), on_message_(std::move(on_message)) {}

WsServer::~WsServer() = default;

Status WsServer::listen(const std::string& addr, std::uint16_t port) {
  Result<std::unique_ptr<TcpListener>> lr = TcpListener::Create(
      loop_, addr, port, [this](int fd) { on_accept(fd); });
  if (!lr.ok()) {
    return lr.error();
  }
  listener_ = std::move(lr.value());
  return Status::Ok();
}

std::uint16_t WsServer::port() const {
  return listener_ ? listener_->port() : 0;
}

void WsServer::on_accept(int fd) {
  const std::uint64_t id = next_conn_id_++;
  auto conn = std::make_unique<WsConnection>(
      loop_, fd, id, on_message_, [this](int cfd) { close_connection(cfd); });
  if (Status s = loop_.add(fd, IoInterest::kRead, conn.get()); !s.ok()) {
    return;  // conn destructs, closing fd.
  }
  conns_.emplace(fd, std::move(conn));
}

void WsServer::deliver_text(int fd, std::uint64_t id, std::string_view text) {
  const auto it = conns_.find(fd);
  if (it == conns_.end() || it->second->id() != id) {
    return;  // connection gone, or fd reused by a newer connection
  }
  it->second->send_text(text);
}

void WsServer::close_connection(int fd) {
  loop_.remove(fd);
  loop_.defer([this, fd]() { conns_.erase(fd); });
}

}  // namespace codicis
