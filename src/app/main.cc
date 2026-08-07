/**
 * @file main.cc
 * @brief codicis entry point: load config, wire the server, run the loop.
 */

#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <memory>

#include "codicis/app/options.h"
#include "codicis/app/server.h"
#include "codicis/config/config.h"
#include "codicis/event/event_loop.h"
#include "codicis/util/clock.h"
#include "codicis/util/logging.h"

namespace {

using namespace codicis;

// Self-pipe write end, set by the SignalStopper and touched by the async signal
// handler (the only async-signal-safe way to reach the loop).
int g_signal_write_fd = -1;

/** @brief Async-signal-safe handler: nudge the self-pipe so the loop wakes. */
void HandleStopSignal(int /*signo*/) {
  if (g_signal_write_fd >= 0) {
    const char byte = 1;
    const ssize_t n = ::write(g_signal_write_fd, &byte, 1);
    (void)n;  // nothing safe to do on failure inside a signal handler
  }
}

/**
 * @brief Stops the event loop on SIGTERM/SIGINT via the self-pipe trick.
 *
 * A signal handler can only safely write() a byte to a pipe; the pipe's read
 * end is a normal loop descriptor, so the wake-up is delivered as an ordinary
 * readable event and turned into loop.stop() on the loop thread.
 */
class SignalStopper final : public IoHandler {
 public:
  explicit SignalStopper(EventLoop& loop) : loop_(loop) {
    int fds[2];
    if (::pipe(fds) != 0) {
      return;
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];
    for (const int fd : {read_fd_, write_fd_}) {
      const int fl = ::fcntl(fd, F_GETFL, 0);
      ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
      const int fdfl = ::fcntl(fd, F_GETFD, 0);
      ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
    }
    g_signal_write_fd = write_fd_;
    loop_.add(read_fd_, IoInterest::kRead, this);

    struct sigaction sa;
    sa.sa_handler = HandleStopSignal;
    sigemptyset(&sa.sa_mask);  // a macro on some platforms; no :: qualifier
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);
  }

  ~SignalStopper() override {
    ::signal(SIGTERM, SIG_DFL);
    ::signal(SIGINT, SIG_DFL);
    g_signal_write_fd = -1;
    if (read_fd_ >= 0) {
      loop_.remove(read_fd_);
      ::close(read_fd_);
    }
    if (write_fd_ >= 0) {
      ::close(write_fd_);
    }
  }

  SignalStopper(const SignalStopper&) = delete;
  SignalStopper& operator=(const SignalStopper&) = delete;

  void on_io_ready(int /*fd*/, IoEvents events) override {
    if (HasEvent(events, IoEvents::kReadable) ||
        HasEvent(events, IoEvents::kHangup)) {
      char buf[16];
      while (::read(read_fd_, buf, sizeof(buf)) > 0) {
      }
      LogMessage(LogLevel::kInfo, "shutdown signal received; stopping loop");
      loop_.stop();
    }
  }

 private:
  EventLoop& loop_;
  int read_fd_ = -1;
  int write_fd_ = -1;
};

}  // namespace

int main(int argc, char** argv) {
  using namespace codicis;

  const OptionRegistry registry = BuildOptionRegistry();
  Result<Config> cfg = Config::load(registry, argc, argv);
  if (!cfg.ok()) {
    std::fprintf(stderr, "configuration error: %s\n",
                 cfg.error().message.c_str());
    return 2;
  }
  const Config& config = cfg.value();

  LogLevel level = LogLevel::kInfo;
  if (ParseLogLevel(config.get_string("log.level").value(), &level)) {
    SetLogLevel(level);
  }

  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> loop = MakeEventLoop(&clock);
  if (!loop.ok()) {
    std::fprintf(stderr, "event loop error: %s\n",
                 loop.error().message.c_str());
    return 1;
  }

  AppServer server(*loop.value(), config);
  if (Status s = server.start(); !s.ok()) {
    std::fprintf(stderr, "startup error: %s\n", s.error().message.c_str());
    return 1;
  }

  // Catch SIGTERM/SIGINT so the loop stops cleanly instead of the process being
  // killed mid-flight; then drain and let RAII tear the helpers down (their
  // stdin closes -> the children see EOF and exit) before returning 0.
  SignalStopper stopper(*loop.value());
  loop.value()->run();
  server.drain(/*max_ms=*/5000);
  return 0;
}
