/**
 * @file test_feed_soak.cc
 * @brief Fan-out load/soak for the market-data feed-helper over the REAL event
 *        loop (epoll on Linux, kqueue elsewhere).
 *
 * Spawns the shipped codicis_feed_helper binary, streams book events into its
 * stdin, and drives many TCP subscribers to pin the fan-out contract:
 *   1. many concurrent subscribers all receive the streamed L1 updates;
 *   2. a slow consumer is DISCONNECTED once it exceeds the per-subscriber
 *      outbound cap, while the other subscribers keep flowing and the
 *      stdin->replica->broadcast hot path never stalls;
 *   3. an abrupt subscriber close mid-broadcast does not disturb the others
 *      (removal is deferred, so the broadcast loop stays safe).
 *
 * The point is to exercise the epoll backend's per-subscriber writable re-arm /
 * backpressure / drop path, which had only ever run on kqueue.
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "codicis/core/book_event.h"
#include "codicis/feed/feed_wire.h"

using namespace codicis;

namespace {

using Clock = std::chrono::steady_clock;

/** @brief Spawn the feed-helper; return its pid and the parent-side pipe fds.
 */
pid_t SpawnHelper(int* child_in, int* child_out) {
  int to_child[2];
  int from_child[2];
  REQUIRE(::pipe(to_child) == 0);
  REQUIRE(::pipe(from_child) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(to_child[0], STDIN_FILENO);
    ::dup2(from_child[1], STDOUT_FILENO);
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    ::execl(CODICIS_FEED_HELPER_PATH, "codicis_feed_helper", "127.0.0.1", "0",
            static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(to_child[0]);
  ::close(from_child[1]);
  *child_in = to_child[1];
  *child_out = from_child[0];
  return pid;
}

/** @brief Read the "listening <port>" banner from the helper's stdout. */
std::uint16_t ReadPort(int child_out) {
  std::string banner;
  for (int i = 0; i < 400 && banner.find("listening ") == std::string::npos;
       ++i) {
    char buf[256];
    const ssize_t n = ::read(child_out, buf, sizeof(buf));
    if (n > 0) {
      banner.append(buf, static_cast<std::size_t>(n));
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      ::usleep(5000);
    } else if (n <= 0) {
      break;
    }
  }
  const std::size_t pos = banner.find("listening ");
  REQUIRE(pos != std::string::npos);
  return static_cast<std::uint16_t>(std::atoi(banner.c_str() + pos + 10));
}

/** @brief A connected, non-blocking subscriber; optionally small-buffered. */
int ConnectSub(std::uint16_t port, int rcvbuf = 0) {
  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(c >= 0);
  if (rcvbuf > 0) {
    ::setsockopt(c, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }
  struct sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  REQUIRE(::connect(c, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) ==
          0);
  const int flags = ::fcntl(c, F_GETFL, 0);
  ::fcntl(c, F_SETFL, flags | O_NONBLOCK);
  return c;
}

/** @brief Count non-overlapping occurrences of @p needle in @p hay. */
int Count(const std::string& hay, const std::string& needle) {
  int n = 0;
  std::size_t pos = 0;
  while ((pos = hay.find(needle, pos)) != std::string::npos) {
    ++n;
    pos += needle.size();
  }
  return n;
}

/** @brief One tracked subscriber: its fd and the bytes it has received. */
struct Sub {
  int fd = -1;
  std::string acc;
  bool eof = false;
};

/** @brief Non-blocking drain of @p s into its accumulator. */
void Drain(Sub& s) {
  for (;;) {
    char buf[16384];
    const ssize_t n = ::recv(s.fd, buf, sizeof(buf), 0);
    if (n > 0) {
      s.acc.append(buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      s.eof = true;
      return;
    }
    return;  // EAGAIN
  }
}

/**
 * @brief True once the helper has closed our end of @p fd.
 *
 * Detected WITHOUT reading the subscriber's inbound data -- reading would let
 * its backlog drain and lift the very backpressure we are trying to build. We
 * poke it with a one-byte command (a blank line the helper ignores): once the
 * helper has dropped and closed the socket, the send fails with EPIPE/RST, or a
 * non-consuming peek observes the orderly EOF.
 */
bool PeerDropped(int fd) {
  const char probe = '\n';
  const ssize_t n = ::send(fd, &probe, 1, MSG_NOSIGNAL);
  if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) {
    return true;
  }
  char b;
  return ::recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

/** @brief Encode @p count Add events on "BTC" (alternating side) into bytes. */
std::string MakeEvents(SeqNo start, int count) {
  std::string bytes;
  for (int i = 0; i < count; ++i) {
    BookEvent e;
    e.seq = start + static_cast<SeqNo>(i);
    e.symbol = "BTC";
    e.type = BookEventType::kAdd;
    e.order_id = static_cast<OrderId>(i) + 1;
    e.side = (i % 2 == 0) ? Side::Buy : Side::Sell;
    e.price = (i % 2 == 0) ? 100 + (i % 5) : 110 + (i % 5);
    e.qty = 1 + (i % 7);
    EncodeBookEvent(e, &bytes);
  }
  return bytes;
}

/** @brief True once every subscriber's accumulated stream contains "ready". */
bool AllReady(std::vector<Sub>& subs) {
  for (Sub& s : subs) {
    Drain(s);
    if (s.acc.find("\"ready\"") == std::string::npos) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("Feed fan-out: many subscribers all receive the stream",
          "[feed][epoll][soak]") {
  int child_in = -1;
  int child_out = -1;
  const pid_t pid = SpawnHelper(&child_in, &child_out);
  const std::uint16_t port = ReadPort(child_out);
  REQUIRE(port != 0);

  constexpr int kSubs = 50;
  constexpr int kEvents = 200;
  std::vector<Sub> subs(kSubs);
  for (Sub& s : subs) {
    s.fd = ConnectSub(port);
  }

  const auto deadline = Clock::now() + std::chrono::seconds(30);
  while (!AllReady(subs)) {
    REQUIRE(Clock::now() < deadline);
  }

  const std::string events = MakeEvents(1, kEvents);
  REQUIRE(::write(child_in, events.data(), events.size()) ==
          static_cast<ssize_t>(events.size()));

  bool all_got = false;
  while (!all_got) {
    all_got = true;
    for (Sub& s : subs) {
      Drain(s);
      if (Count(s.acc, "\"t\":\"l1\"") < kEvents) {
        all_got = false;
      }
    }
    REQUIRE(Clock::now() < deadline);
  }
  SUCCEED("all subscribers received the full stream");

  for (Sub& s : subs) {
    ::close(s.fd);
  }
  ::close(child_in);
  ::close(child_out);
  ::waitpid(pid, nullptr, 0);
}

TEST_CASE("Feed fan-out: a slow consumer is dropped, others keep flowing",
          "[feed][epoll][soak]") {
  int child_in = -1;
  int child_out = -1;
  const pid_t pid = SpawnHelper(&child_in, &child_out);
  const std::uint16_t port = ReadPort(child_out);
  REQUIRE(port != 0);

  // A few fast subscribers we keep draining, plus one wedged slow subscriber
  // with a tiny receive buffer. We NEVER read the slow one (reading would let
  // its backlog drain and lift the backpressure); its disconnection is detected
  // by probing (see PeerDropped).
  constexpr int kFast = 3;
  std::vector<Sub> fast(kFast);
  for (Sub& s : fast) {
    s.fd = ConnectSub(port);
  }
  const int slow = ConnectSub(port, /*rcvbuf=*/4096);

  const auto deadline = Clock::now() + std::chrono::seconds(60);
  while (!AllReady(fast)) {
    REQUIRE(Clock::now() < deadline);
  }
  {  // greet the slow subscriber once, then leave it wedged
    std::string g;
    for (int i = 0; i < 400 && g.find("\"ready\"") == std::string::npos; ++i) {
      char b[512];
      const ssize_t n = ::recv(slow, b, sizeof(b), 0);
      if (n > 0) {
        g.append(b, static_cast<std::size_t>(n));
      } else {
        ::usleep(3000);
      }
    }
    REQUIRE(g.find("\"ready\"") != std::string::npos);
  }

  // Push well past the 4 MiB per-subscriber cap plus the kernel send buffer
  // (tcp_wmem max, ~4 MiB) so the wedged subscriber's out buffer must overflow.
  constexpr int kEvents = 150000;
  const std::string events = MakeEvents(1, kEvents);

  const int fl = ::fcntl(child_in, F_GETFL, 0);
  ::fcntl(child_in, F_SETFL, fl | O_NONBLOCK);
  std::size_t sent = 0;
  bool slow_dropped = false;
  while (sent < events.size()) {
    const ssize_t n =
        ::write(child_in, events.data() + sent, events.size() - sent);
    if (n > 0) {
      sent += static_cast<std::size_t>(n);
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      for (Sub& s : fast) {
        Drain(s);  // keep fast consumers moving so they are not dropped
      }
      if (!slow_dropped && PeerDropped(slow)) {
        slow_dropped = true;
      }
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
    REQUIRE(Clock::now() < deadline);
  }
  // (c) The hot path never blocked: the helper drained every byte of stdin.
  REQUIRE(sent == events.size());

  // Keep the other subscribers flowing while we wait for the slow one to drop.
  while (!slow_dropped) {
    for (Sub& s : fast) {
      Drain(s);
    }
    if (PeerDropped(slow)) {
      slow_dropped = true;
    }
    REQUIRE(Clock::now() < deadline);
    if (!slow_dropped) {
      ::usleep(2000);
    }
  }
  REQUIRE(slow_dropped);  // (a) the slow consumer was disconnected

  // (b) The fast subscribers kept receiving the entire stream, undisturbed.
  bool all_full = false;
  while (!all_full) {
    all_full = true;
    for (Sub& s : fast) {
      Drain(s);
      if (Count(s.acc, "\"t\":\"l1\"") < kEvents) {
        all_full = false;
      }
    }
    REQUIRE(Clock::now() < deadline);
  }

  // (d) The helper is still healthy: a brand-new subscriber is greeted, proving
  // the broadcast/accept path never wedged.
  Sub late;
  late.fd = ConnectSub(port);
  std::vector<Sub> just_late{late};
  while (!AllReady(just_late)) {
    REQUIRE(Clock::now() < deadline);
  }

  ::close(late.fd);
  ::close(slow);
  for (Sub& s : fast) {
    ::close(s.fd);
  }
  ::close(child_in);
  ::close(child_out);
  ::waitpid(pid, nullptr, 0);
}

TEST_CASE("Feed fan-out: abrupt subscriber close mid-broadcast is safe",
          "[feed][epoll][soak]") {
  int child_in = -1;
  int child_out = -1;
  const pid_t pid = SpawnHelper(&child_in, &child_out);
  const std::uint16_t port = ReadPort(child_out);
  REQUIRE(port != 0);

  constexpr int kSubs = 8;
  std::vector<Sub> subs(kSubs);
  for (Sub& s : subs) {
    s.fd = ConnectSub(port);
  }
  const auto deadline = Clock::now() + std::chrono::seconds(30);
  while (!AllReady(subs)) {
    REQUIRE(Clock::now() < deadline);
  }

  // Stream in two halves; between them, abruptly close half the subscribers.
  const std::string first = MakeEvents(1, 500);
  REQUIRE(::write(child_in, first.data(), first.size()) ==
          static_cast<ssize_t>(first.size()));
  for (int i = 0; i < 100; ++i) {
    for (Sub& s : subs) {
      Drain(s);
    }
    ::usleep(1000);
  }
  // Yank the odd-indexed subscribers without any goodbye.
  for (int i = 1; i < kSubs; i += 2) {
    ::close(subs[static_cast<std::size_t>(i)].fd);
    subs[static_cast<std::size_t>(i)].fd = -1;
  }

  const std::string second = MakeEvents(501, 500);
  REQUIRE(::write(child_in, second.data(), second.size()) ==
          static_cast<ssize_t>(second.size()));

  // The survivors must receive the full 1000-event stream unperturbed.
  bool done = false;
  while (!done) {
    done = true;
    for (Sub& s : subs) {
      if (s.fd < 0) {
        continue;
      }
      Drain(s);
      if (Count(s.acc, "\"t\":\"l1\"") < 1000) {
        done = false;
      }
    }
    REQUIRE(Clock::now() < deadline);
  }
  SUCCEED("survivors received the full stream after abrupt peer closes");

  for (Sub& s : subs) {
    if (s.fd >= 0) {
      ::close(s.fd);
    }
  }
  ::close(child_in);
  ::close(child_out);
  ::waitpid(pid, nullptr, 0);
}
