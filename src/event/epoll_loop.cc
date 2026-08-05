/**
 * @file epoll_loop.cc
 * @brief Implementation of the epoll EventLoop backend (see epoll_loop.h).
 */

#include "epoll_loop.h"

#if defined(CODICIS_HAVE_EPOLL)

#include <sys/epoll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace codicis {
namespace {

/** @brief Maximum events drained from the kernel per poll. */
constexpr int kMaxEvents = 64;

/**
 * @brief Build a strerror-based Error for a failing syscall.
 * @param what Short description of the failed operation.
 * @return An Error with code kIo.
 */
Error SyscallError(const char* what) {
  return MakeError(ErrorCode::kIo,
                   std::string(what) + ": " + std::strerror(errno));
}

/**
 * @brief Translate an IoInterest mask into epoll event flags.
 * @param interest The requested readiness.
 * @return The corresponding EPOLLIN/EPOLLOUT bitmask (level-triggered).
 */
uint32_t ToEpollFlags(IoInterest interest) {
  uint32_t flags = 0;
  if (HasInterest(interest, IoInterest::kRead)) {
    flags |= EPOLLIN;
  }
  if (HasInterest(interest, IoInterest::kWrite)) {
    flags |= EPOLLOUT;
  }
  return flags;
}

}  // namespace

Result<std::unique_ptr<EpollLoop>> EpollLoop::Create(Clock* clock) {
  const int epfd = ::epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) {
    return SyscallError("epoll_create1");
  }
  return std::unique_ptr<EpollLoop>(new EpollLoop(clock, epfd));
}

EpollLoop::~EpollLoop() {
  if (epfd_ >= 0) {
    ::close(epfd_);
  }
}

Status EpollLoop::backend_add(int fd, IoInterest interest) {
  struct epoll_event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.events = ToEpollFlags(interest);
  ev.data.fd = fd;
  if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
    return Status(SyscallError("epoll_ctl(add)"));
  }
  return Status::Ok();
}

Status EpollLoop::backend_modify(int fd, IoInterest interest) {
  struct epoll_event ev;
  std::memset(&ev, 0, sizeof(ev));
  ev.events = ToEpollFlags(interest);
  ev.data.fd = fd;
  if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
    return Status(SyscallError("epoll_ctl(mod)"));
  }
  return Status::Ok();
}

Status EpollLoop::backend_remove(int fd) {
  if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT) {
    return Status(SyscallError("epoll_ctl(del)"));
  }
  return Status::Ok();
}

Status EpollLoop::backend_poll(int timeout_ms) {
  std::array<struct epoll_event, kMaxEvents> events;
  const int n =
      ::epoll_wait(epfd_, events.data(), kMaxEvents, timeout_ms);
  if (n < 0) {
    if (errno == EINTR) {
      return Status::Ok();  // Interrupted; caller loops again.
    }
    return Status(SyscallError("epoll_wait"));
  }

  for (int i = 0; i < n; ++i) {
    const struct epoll_event& ev = events[static_cast<std::size_t>(i)];
    const bool hangup = (ev.events & (EPOLLHUP | EPOLLRDHUP)) != 0;
    IoEvents out = IoEvents::kNone;
    // kReadable is documented as "data available OR EOF pending" (event_loop.h).
    // A pure EPOLLHUP (e.g. a pipe whose writer closed with an empty buffer)
    // means EOF is pending, but epoll does not co-set EPOLLIN in that case, so
    // surface it as readable too. This mirrors kqueue, where EV_EOF fires
    // alongside the read filter; without it a handler that only checks
    // kReadable would never run read()/observe the EOF and would hang.
    if ((ev.events & EPOLLIN) != 0 || hangup) {
      out |= IoEvents::kReadable;
    }
    if ((ev.events & EPOLLOUT) != 0) {
      out |= IoEvents::kWritable;
    }
    if ((ev.events & EPOLLERR) != 0) {
      out |= IoEvents::kError;
    }
    if (hangup) {
      out |= IoEvents::kHangup;
    }
    dispatch_io(ev.data.fd, out);
  }
  return Status::Ok();
}

}  // namespace codicis

#endif  // CODICIS_HAVE_EPOLL
