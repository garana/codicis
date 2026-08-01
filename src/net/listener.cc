/**
 * @file listener.cc
 * @brief Implementation of TcpListener (see listener.h).
 */

#include "codicis/net/listener.h"

#include <cerrno>

#include "socket_util.h"

namespace codicis {

Result<std::unique_ptr<TcpListener>> TcpListener::Create(
    EventLoop& loop, const std::string& addr, std::uint16_t port,
    AcceptFn on_accept) {
  int fd = -1;
  std::uint16_t bound_port = 0;
  if (Status s = net_internal::CreateListenSocket(addr, port, &fd,
                                                  &bound_port);
      !s.ok()) {
    return s.error();
  }

  auto listener = std::unique_ptr<TcpListener>(
      new TcpListener(loop, fd, bound_port, std::move(on_accept)));
  if (Status s = loop.add(fd, IoInterest::kRead, listener.get()); !s.ok()) {
    ::close(fd);
    return s.error();
  }
  return listener;
}

TcpListener::~TcpListener() {
  if (fd_ >= 0) {
    loop_.remove(fd_);
    ::close(fd_);
  }
}

void TcpListener::on_io_ready(int /*fd*/, IoEvents /*events*/) {
  // Accept until the backlog drains (level-triggered: any remainder fires
  // again on the next loop iteration).
  for (;;) {
    const int conn = ::accept(fd_, nullptr, nullptr);
    if (conn < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      break;  // Unexpected error; stop this cycle.
    }
    if (Status s = net_internal::SetNonBlocking(conn); !s.ok()) {
      ::close(conn);
      continue;
    }
    on_accept_(conn);
  }
}

}  // namespace codicis
