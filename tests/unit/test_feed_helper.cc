/**
 * @file test_feed_helper.cc
 * @brief Integration test: spawn the feed-helper, stream events, fan out to a
 *        TCP subscriber.
 *
 * Exercises the shipped binary end to end -- stdin event stream -> BookReplica
 * -> event-loop TCP fan-out -- over the generic EventLoop backend of the host
 * (kqueue here; the epoll backend is validated on Linux).
 */

#include "catch_amalgamated.hpp"

#include "codicis/core/book_event.h"
#include "codicis/feed/feed_wire.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

using namespace codicis;

namespace {

BookEvent Add(SeqNo seq, const Symbol& sym, OrderId id, Side side, Ticks px,
              Quantity qty) {
  BookEvent e;
  e.seq = seq;
  e.symbol = sym;
  e.type = BookEventType::kAdd;
  e.order_id = id;
  e.side = side;
  e.price = px;
  e.qty = qty;
  return e;
}

/** @brief Read available bytes into @p acc until it contains @p needle or a
 *  bounded number of attempts elapse. */
bool ReadUntil(int fd, std::string& acc, const std::string& needle) {
  for (int i = 0; i < 200 && acc.find(needle) == std::string::npos; ++i) {
    char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      acc.append(buf, static_cast<std::size_t>(n));
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      ::usleep(5000);
    } else if (n <= 0) {
      break;
    }
  }
  return acc.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("Feed-helper fans book events out to a TCP subscriber",
          "[feed][helper]") {
  int to_child[2];   // parent -> child stdin (events)
  int from_child[2]; // child stdout -> parent (the "listening <port>" line)
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
  const int child_in = to_child[1];    // we write events here
  const int child_out = from_child[0];  // we read the port here

  // Learn the bound (ephemeral) port from the child's stdout.
  std::string banner;
  REQUIRE(ReadUntil(child_out, banner, "listening "));
  const std::size_t pos = banner.find("listening ");
  const std::uint16_t port =
      static_cast<std::uint16_t>(std::atoi(banner.c_str() + pos + 10));
  REQUIRE(port != 0);

  // Connect a subscriber with a receive timeout so the test cannot hang.
  const int c = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(c >= 0);
  struct sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  REQUIRE(::connect(c, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) ==
          0);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 200000;  // 200 ms: a stalled read retries rather than hanging
  ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // The greeting is a snapshot (empty book) then {"t":"ready"}.
  std::string stream;
  REQUIRE(ReadUntil(c, stream, "\"ready\""));

  // Stream a resting sell then a resting buy; expect an L1 update reflecting
  // the best bid/ask.
  std::string bytes;
  EncodeBookEvent(Add(1, "BTC", 1, Side::Sell, 105, 8), &bytes);
  EncodeBookEvent(Add(2, "BTC", 2, Side::Buy, 101, 3), &bytes);
  REQUIRE(::write(child_in, bytes.data(), bytes.size()) ==
          static_cast<ssize_t>(bytes.size()));

  // The second event's L1 line carries both the bid and the (still-resting)
  // ask; wait for it so both are present in the accumulated stream.
  REQUIRE(ReadUntil(c, stream, "\"bid\":101"));
  REQUIRE(stream.find("\"ask\":105") != std::string::npos);

  // An L2 query returns aggregated depth.
  const std::string q = "l2 BTC\n";
  REQUIRE(::write(c, q.data(), q.size()) == static_cast<ssize_t>(q.size()));
  REQUIRE(ReadUntil(c, stream, "\"t\":\"l2\""));
  REQUIRE(stream.find("[105,8]") != std::string::npos);

  ::close(c);
  ::close(child_in);
  ::close(child_out);
  int status = 0;
  ::waitpid(pid, &status, 0);
}
