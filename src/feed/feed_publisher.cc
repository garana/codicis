/**
 * @file feed_publisher.cc
 * @brief Implementation of FeedPublisher and SpawnFeedHelper.
 */

#include "codicis/feed/feed_publisher.h"

#include <fcntl.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "codicis/feed/feed_wire.h"

namespace codicis {
namespace {

/** @brief Cap on the un-drained outbound buffer; overflow drops events. */
constexpr std::size_t kOutCap = std::size_t{16} * 1024 * 1024;

/** @brief Put a descriptor into non-blocking, close-on-exec mode. */
void MakeCloexecNonBlocking(int fd) {
  const int fl = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  const int fd_flags = ::fcntl(fd, F_GETFD, 0);
  ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
}

}  // namespace

FeedPublisher::FeedPublisher(EventLoop& loop, int write_fd)
    : loop_(loop), write_fd_(write_fd) {
  static bool sigpipe_ignored = false;
  if (!sigpipe_ignored) {
    ::signal(SIGPIPE, SIG_IGN);
    sigpipe_ignored = true;
  }
}

FeedPublisher::~FeedPublisher() {
  if (!closed_ && write_armed_) {
    loop_.remove(write_fd_);
  }
  if (write_fd_ >= 0) {
    ::close(write_fd_);
  }
  if (child_pid_ > 0) {
    int status = 0;
    ::waitpid(child_pid_, &status, WNOHANG);
  }
}

void FeedPublisher::on_book_event(const BookEvent& ev) {
  if (closed_) {
    return;
  }
  std::string bytes;
  EncodeBookEvent(ev, &bytes);
  // Best-effort: never grow without bound and never block the matcher. A full
  // buffer means the helper is behind -- drop the event; the seq gap it leaves
  // is what triggers the helper's resync.
  if (out_.size() + bytes.size() > kOutCap) {
    ++dropped_;
    return;
  }
  out_.append(bytes);
  arm_write(true);
  flush();
}

void FeedPublisher::on_io_ready(int fd, IoEvents events) {
  if (fd == write_fd_ && HasEvent(events, IoEvents::kWritable)) {
    flush();
    if (closed_) {
      return;
    }
  }
  if (HasEvent(events, IoEvents::kError)) {
    handle_close();
  }
}

void FeedPublisher::flush() {
  while (!out_.empty()) {
    const ssize_t n = ::write(write_fd_, out_.data(), out_.size());
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
    handle_close();  // helper gone (EPIPE etc.)
    return;
  }
  if (out_.empty()) {
    arm_write(false);
  }
}

void FeedPublisher::arm_write(bool on) {
  if (closed_ || on == write_armed_) {
    return;
  }
  write_armed_ = on;
  if (on) {
    loop_.add(write_fd_, IoInterest::kWrite, this);
  } else {
    loop_.remove(write_fd_);
  }
}

void FeedPublisher::handle_close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (write_armed_) {
    loop_.remove(write_fd_);
    write_armed_ = false;
  }
  out_ = Buffer();  // drop anything undrained; the feed is best-effort
}

Result<std::unique_ptr<FeedPublisher>> SpawnFeedHelper(
    EventLoop& loop, const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return MakeError(ErrorCode::kInvalidArg, "SpawnFeedHelper: empty argv");
  }

  int p2c[2];  // parent -> child (child stdin): codicis writes the event stream
  if (::pipe(p2c) < 0) {
    return MakeError(ErrorCode::kIo,
                     std::string("pipe: ") + std::strerror(errno));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    const int e = errno;
    ::close(p2c[0]);
    ::close(p2c[1]);
    return MakeError(ErrorCode::kIo,
                     std::string("fork: ") + std::strerror(e));
  }

  if (pid == 0) {
    // Child: wire stdin to the pipe; inherit stdout/stderr (banner + logs).
    ::dup2(p2c[0], STDIN_FILENO);
    ::close(p2c[0]);
    ::close(p2c[1]);
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& a : argv) {
      cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);
    ::execvp(cargv[0], cargv.data());
    ::_exit(127);  // exec failed
  }

  ::close(p2c[0]);
  const int write_fd = p2c[1];
  MakeCloexecNonBlocking(write_fd);
  auto pub = std::make_unique<FeedPublisher>(loop, write_fd);
  pub->set_child_pid(pid);
  return pub;
}

}  // namespace codicis
