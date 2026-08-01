/**
 * @file kqueue_loop.cc
 * @brief Implementation of the kqueue EventLoop backend (see kqueue_loop.h).
 */

#include "kqueue_loop.h"

#if defined(CODICIS_HAVE_KQUEUE)

#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
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

}  // namespace

Result<std::unique_ptr<KqueueLoop>> KqueueLoop::Create(Clock* clock) {
  const int kq = ::kqueue();
  if (kq < 0) {
    return SyscallError("kqueue");
  }
  ::fcntl(kq, F_SETFD, FD_CLOEXEC);
  return std::unique_ptr<KqueueLoop>(new KqueueLoop(clock, kq));
}

KqueueLoop::~KqueueLoop() {
  if (kq_ >= 0) {
    ::close(kq_);
  }
}

Status KqueueLoop::apply_interest(int fd, IoInterest interest) {
  // EV_ADD is idempotent, so the same change list serves both add and modify:
  // each filter is (re)registered and enabled or disabled to match interest.
  struct kevent changes[2];
  const unsigned short read_flags = static_cast<unsigned short>(
      EV_ADD | (HasInterest(interest, IoInterest::kRead) ? EV_ENABLE
                                                         : EV_DISABLE));
  const unsigned short write_flags = static_cast<unsigned short>(
      EV_ADD | (HasInterest(interest, IoInterest::kWrite) ? EV_ENABLE
                                                          : EV_DISABLE));
  EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, read_flags, 0,
         0, nullptr);
  EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, write_flags, 0,
         0, nullptr);
  if (::kevent(kq_, changes, 2, nullptr, 0, nullptr) < 0) {
    return Status(SyscallError("kevent(add)"));
  }
  return Status::Ok();
}

Status KqueueLoop::backend_add(int fd, IoInterest interest) {
  return apply_interest(fd, interest);
}

Status KqueueLoop::backend_modify(int fd, IoInterest interest) {
  return apply_interest(fd, interest);
}

Status KqueueLoop::backend_remove(int fd) {
  struct kevent changes[2];
  EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0,
         0, nullptr);
  EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0,
         0, nullptr);
  // Ignore ENOENT/EBADF: a closed fd is auto-removed by the kernel.
  ::kevent(kq_, changes, 2, nullptr, 0, nullptr);
  return Status::Ok();
}

Status KqueueLoop::backend_poll(int timeout_ms) {
  std::array<struct kevent, kMaxEvents> events;
  struct timespec ts;
  struct timespec* tsp = nullptr;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000L;
    tsp = &ts;
  }

  const int n = ::kevent(kq_, nullptr, 0, events.data(), kMaxEvents, tsp);
  if (n < 0) {
    if (errno == EINTR) {
      return Status::Ok();  // Interrupted; caller loops again.
    }
    return Status(SyscallError("kevent(poll)"));
  }

  for (int i = 0; i < n; ++i) {
    const struct kevent& ev = events[static_cast<std::size_t>(i)];
    const int fd = static_cast<int>(ev.ident);
    IoEvents out = IoEvents::kNone;
    if ((ev.flags & EV_ERROR) != 0) {
      out |= IoEvents::kError;
    }
    if (ev.filter == EVFILT_READ) {
      out |= IoEvents::kReadable;
    } else if (ev.filter == EVFILT_WRITE) {
      out |= IoEvents::kWritable;
    }
    if ((ev.flags & EV_EOF) != 0) {
      out |= IoEvents::kHangup;
    }
    dispatch_io(fd, out);
  }
  return Status::Ok();
}

}  // namespace codicis

#endif  // CODICIS_HAVE_KQUEUE
