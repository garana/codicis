/**
 * @file ingress_helper.cc
 * @brief Implementation of IngressHelper and SpawnIngressHelper.
 */

#include "codicis/ipc/ingress_helper.h"

#include <fcntl.h>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace codicis {
namespace {

constexpr std::size_t kReadChunk = 16 * 1024;

/** @brief Put a descriptor into non-blocking, close-on-exec mode. */
void MakeCloexecNonBlocking(int fd) {
  const int fl = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  const int fd_flags = ::fcntl(fd, F_GETFD, 0);
  ::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
}

}  // namespace

IngressHelper::IngressHelper(EventLoop& loop, int read_fd, int write_fd,
                             const HelperCodec& codec, DispatchFn dispatch)
    : loop_(loop),
      read_fd_(read_fd),
      write_fd_(write_fd),
      same_fd_(read_fd == write_fd),
      codec_(codec),
      dispatch_(std::move(dispatch)) {
  static bool sigpipe_ignored = false;
  if (!sigpipe_ignored) {
    ::signal(SIGPIPE, SIG_IGN);
    sigpipe_ignored = true;
  }
  loop_.add(read_fd_, IoInterest::kRead, this);
}

IngressHelper::~IngressHelper() {
  if (!closed_) {
    loop_.remove(read_fd_);
    if (write_registered_) {
      loop_.remove(write_fd_);
    }
  }
  if (read_fd_ >= 0) {
    ::close(read_fd_);
  }
  if (!same_fd_ && write_fd_ >= 0) {
    ::close(write_fd_);
  }
  if (child_pid_ > 0) {
    int status = 0;
    ::waitpid(child_pid_, &status, WNOHANG);
  }
}

void IngressHelper::on_io_ready(int fd, IoEvents events) {
  if (fd == read_fd_ && HasEvent(events, IoEvents::kReadable)) {
    on_readable();
    if (closed_) {
      return;
    }
  }
  if (fd == write_fd_ && HasEvent(events, IoEvents::kWritable)) {
    on_writable();
    if (closed_) {
      return;
    }
  }
  if (HasEvent(events, IoEvents::kError)) {
    handle_close();
  }
}

void IngressHelper::on_readable() {
  for (;;) {
    std::uint8_t* dst = in_.reserve(kReadChunk);
    const ssize_t n = ::read(read_fd_, dst, kReadChunk);
    if (n > 0) {
      in_.commit(static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      handle_close();
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    handle_close();
    return;
  }

  for (;;) {
    HelperMessage msg;
    std::string err;
    const HelperDecode d = codec_.decode(in_, &msg, &err);
    if (d == HelperDecode::kIncomplete) {
      break;
    }
    if (d == HelperDecode::kError) {
      handle_close();
      return;
    }
    // Stamp the request's id onto whatever reply the handler produces so the
    // helper can correlate it (replies may arrive later and out of order).
    const std::uint64_t req_id = msg.req_id;
    dispatch_(msg, [this, req_id](HelperMessage reply) {
      reply.req_id = req_id;
      queue_reply(std::move(reply));
    });
    if (closed_) {
      return;
    }
  }
}

void IngressHelper::queue_reply(HelperMessage msg) {
  if (closed_) {
    return;  // helper gone; drop the (now undeliverable) reply
  }
  std::string bytes;
  codec_.encode(msg, &bytes);
  out_.append(bytes);
  set_write_interest(true);
  on_writable();
}

void IngressHelper::on_writable() {
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
    handle_close();
    return;
  }
  if (out_.empty()) {
    set_write_interest(false);
  }
}

void IngressHelper::set_write_interest(bool on) {
  if (same_fd_) {
    if (on == write_enabled_) {
      return;
    }
    write_enabled_ = on;
    loop_.modify(read_fd_, on ? (IoInterest::kRead | IoInterest::kWrite)
                              : IoInterest::kRead);
    return;
  }
  if (on && !write_registered_) {
    loop_.add(write_fd_, IoInterest::kWrite, this);
    write_registered_ = true;
  } else if (!on && write_registered_) {
    loop_.remove(write_fd_);
    write_registered_ = false;
  }
}

void IngressHelper::handle_close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  loop_.remove(read_fd_);
  if (write_registered_) {
    loop_.remove(write_fd_);
    write_registered_ = false;
  }
}

Result<std::unique_ptr<IngressHelper>> SpawnIngressHelper(
    EventLoop& loop, const std::vector<std::string>& argv,
    const HelperCodec& codec, IngressHelper::DispatchFn dispatch) {
  if (argv.empty()) {
    return MakeError(ErrorCode::kInvalidArg, "SpawnIngressHelper: empty argv");
  }

  int p2c[2];  // parent -> child (child stdin): codicis writes replies
  int c2p[2];  // child -> parent (child stdout): codicis reads requests
  if (::pipe(p2c) < 0) {
    return MakeError(ErrorCode::kIo,
                     std::string("pipe: ") + std::strerror(errno));
  }
  if (::pipe(c2p) < 0) {
    const int e = errno;
    ::close(p2c[0]);
    ::close(p2c[1]);
    return MakeError(ErrorCode::kIo,
                     std::string("pipe: ") + std::strerror(e));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    const int e = errno;
    ::close(p2c[0]);
    ::close(p2c[1]);
    ::close(c2p[0]);
    ::close(c2p[1]);
    return MakeError(ErrorCode::kIo,
                     std::string("fork: ") + std::strerror(e));
  }

  if (pid == 0) {
    // Child: wire stdin/stdout to the pipes, exec the helper.
    ::dup2(p2c[0], STDIN_FILENO);
    ::dup2(c2p[1], STDOUT_FILENO);
    ::close(p2c[0]);
    ::close(p2c[1]);
    ::close(c2p[0]);
    ::close(c2p[1]);
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& a : argv) {
      cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);
    ::execvp(cargv[0], cargv.data());
    ::_exit(127);  // exec failed
  }

  // Parent: keep the write end of p2c and the read end of c2p.
  ::close(p2c[0]);
  ::close(c2p[1]);
  const int write_fd = p2c[1];
  const int read_fd = c2p[0];
  MakeCloexecNonBlocking(write_fd);
  MakeCloexecNonBlocking(read_fd);

  auto helper = std::make_unique<IngressHelper>(loop, read_fd, write_fd, codec,
                                                std::move(dispatch));
  helper->set_child_pid(pid);
  return helper;
}

}  // namespace codicis
