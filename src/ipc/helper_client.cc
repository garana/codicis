/**
 * @file helper_client.cc
 * @brief Implementation of HelperClient and SpawnHelper (see helper_client.h).
 */

#include "codicis/ipc/helper_client.h"

#include <fcntl.h>
#include <signal.h>
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

HelperClient::HelperClient(EventLoop& loop, int read_fd, int write_fd,
                           const HelperCodec& codec)
    : loop_(loop),
      read_fd_(read_fd),
      write_fd_(write_fd),
      same_fd_(read_fd == write_fd),
      codec_(codec) {
  static bool sigpipe_ignored = false;
  if (!sigpipe_ignored) {
    ::signal(SIGPIPE, SIG_IGN);
    sigpipe_ignored = true;
  }
  loop_.add(read_fd_, IoInterest::kRead, this);
}

HelperClient::~HelperClient() {
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

std::uint64_t HelperClient::send(
    std::string type,
    std::vector<std::pair<std::string, std::string>> fields, ResponseFn cb) {
  const std::uint64_t id = next_req_id_++;
  HelperMessage msg;
  msg.req_id = id;
  msg.type = std::move(type);
  msg.fields = std::move(fields);

  std::string bytes;
  codec_.encode(msg, &bytes);
  out_.append(bytes);
  if (cb) {
    pending_.emplace(id, std::move(cb));
  }
  set_write_interest(true);
  on_writable();
  return id;
}

void HelperClient::on_io_ready(int fd, IoEvents events) {
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

void HelperClient::on_readable() {
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
    const auto it = pending_.find(msg.req_id);
    if (it != pending_.end()) {
      ResponseFn cb = std::move(it->second);
      pending_.erase(it);
      cb(true, msg);
    }
    // Unsolicited messages (unknown req_id) are ignored for now.
  }
}

void HelperClient::on_writable() {
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

void HelperClient::set_write_interest(bool on) {
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

void HelperClient::handle_close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  loop_.remove(read_fd_);
  if (write_registered_) {
    loop_.remove(write_fd_);
    write_registered_ = false;
  }
  // Fail any in-flight requests.
  std::unordered_map<std::uint64_t, ResponseFn> pending;
  pending.swap(pending_);
  HelperMessage empty;
  for (auto& kv : pending) {
    if (kv.second) {
      kv.second(false, empty);
    }
  }
}

Result<std::unique_ptr<HelperClient>> SpawnHelper(
    EventLoop& loop, const std::vector<std::string>& argv,
    const HelperCodec& codec) {
  if (argv.empty()) {
    return MakeError(ErrorCode::kInvalidArg, "SpawnHelper: empty argv");
  }

  int p2c[2];  // parent -> child (child stdin)
  int c2p[2];  // child -> parent (child stdout)
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

  auto client =
      std::make_unique<HelperClient>(loop, read_fd, write_fd, codec);
  client->set_child_pid(pid);
  return client;
}

}  // namespace codicis
