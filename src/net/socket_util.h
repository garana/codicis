#ifndef CODICIS_NET_SOCKET_UTIL_H
#define CODICIS_NET_SOCKET_UTIL_H

/**
 * @file socket_util.h
 * @brief Internal BSD-socket helpers for the net subsystem.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include "codicis/util/result.h"

namespace codicis {
namespace net_internal {

/**
 * @brief Put a descriptor into non-blocking mode.
 * @param fd The descriptor.
 * @return Ok, or an Error on failure.
 */
inline Status SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return Status(MakeError(ErrorCode::kIo,
                            std::string("fcntl: ") + std::strerror(errno)));
  }
  return Status::Ok();
}

/**
 * @brief Create a non-blocking IPv4 listening socket bound to addr:port.
 * @param addr      Dotted IPv4 address to bind (e.g. "127.0.0.1").
 * @param port      Port to bind; 0 selects an ephemeral port.
 * @param out_fd    Receives the listening descriptor on success.
 * @param out_port  Receives the actual bound port on success.
 * @return Ok, or an Error (with the fd closed) on failure.
 */
inline Status CreateListenSocket(const std::string& addr, std::uint16_t port,
                                 int* out_fd, std::uint16_t* out_port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return Status(MakeError(ErrorCode::kIo,
                            std::string("socket: ") + std::strerror(errno)));
  }
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (::inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
    ::close(fd);
    return Status(MakeError(ErrorCode::kInvalidArg,
                            "invalid bind address: " + addr));
  }
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) < 0) {
    Status s(MakeError(ErrorCode::kIo,
                       std::string("bind: ") + std::strerror(errno)));
    ::close(fd);
    return s;
  }
  if (::listen(fd, 128) < 0) {
    Status s(MakeError(ErrorCode::kIo,
                       std::string("listen: ") + std::strerror(errno)));
    ::close(fd);
    return s;
  }

  struct sockaddr_in bound;
  socklen_t len = sizeof(bound);
  std::memset(&bound, 0, sizeof(bound));
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound), &len) ==
      0) {
    *out_port = ntohs(bound.sin_port);
  } else {
    *out_port = port;
  }

  if (Status s = SetNonBlocking(fd); !s.ok()) {
    ::close(fd);
    return s;
  }
  *out_fd = fd;
  return Status::Ok();
}

}  // namespace net_internal
}  // namespace codicis

#endif  // CODICIS_NET_SOCKET_UTIL_H
