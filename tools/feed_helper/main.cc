/**
 * @file main.cc
 * @brief Reference market-data feed-helper: book-event stream -> L1/L2/L3 fan-out.
 *
 * A spawned child that is the SINK of codicis's book-event stream. It reads
 * @ref codicis::BookEvent records from stdin (the feed wire codec), applies them
 * to a @ref codicis::BookReplica to maintain L1/L2/L3 per symbol, and fans the
 * result out to many TCP subscribers over the generic @ref codicis::EventLoop
 * (kqueue or epoll -- no backend-specific code here).
 *
 * Subscriber protocol (newline-delimited JSON, plaintext -- restrict at the
 * edge / bind private):
 *   - On connect: one {"t":"l1",...} snapshot line per symbol, then
 *     {"t":"ready"}.
 *   - On every applied event: the affected symbol's updated {"t":"l1",...}.
 *   - A subscriber may send commands (newline-terminated):
 *       l2 <sym>              -> {"t":"l2","sym":..,"bids":[[px,qty]..],"asks":..}
 *       l3 <sym> <b|s> <px>   -> {"t":"l3",...,"orders":[[id,qty]..]}
 *
 * Drop policy: each subscriber has a bounded outbound buffer. If an update would
 * overflow it, that subscriber is DISCONNECTED (drop-subscriber, not
 * drop-oldest, not backpressure) so the stdin->replica->broadcast hot path never
 * stalls; a dropped subscriber reconnects and re-snapshots. Gaps in the upstream
 * seq (best-effort feed) are counted and logged; snapshot-based resync of the
 * replica itself is a follow-on.
 *
 * Listen address: argv "<host> <port>" or $CODICIS_FEED_LISTEN ("host:port");
 * default 127.0.0.1:0 (ephemeral). The bound port is printed to stdout as
 * "listening <port>" once ready.
 */

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "codicis/event/event_loop.h"
#include "codicis/feed/book_replica.h"
#include "codicis/feed/feed_wire.h"
#include "codicis/net/listener.h"
#include "codicis/util/buffer.h"
#include "codicis/util/clock.h"

namespace codicis {
namespace {

constexpr std::size_t kReadChunk = std::size_t{64} * 1024;
/** @brief Per-subscriber outbound cap; overflow disconnects the subscriber. */
constexpr std::size_t kSubscriberOutCap = std::size_t{4} * 1024 * 1024;

/** @brief Render an L1 snapshot line for one symbol. */
std::string L1Json(const BookReplica& r, const Symbol& sym) {
  Ticks bpx = 0;
  Quantity bqty = 0;
  Ticks apx = 0;
  Quantity aqty = 0;
  const bool have_bid = r.best_bid(sym, &bpx, &bqty);
  const bool have_ask = r.best_ask(sym, &apx, &aqty);
  std::string s = "{\"t\":\"l1\",\"sym\":\"";
  s += sym;
  s += "\",\"seq\":";
  s += std::to_string(r.last_seq());
  s += ",\"bid\":";
  s += have_bid ? std::to_string(bpx) : "null";
  s += ",\"bid_qty\":";
  s += std::to_string(have_bid ? bqty : 0);
  s += ",\"ask\":";
  s += have_ask ? std::to_string(apx) : "null";
  s += ",\"ask_qty\":";
  s += std::to_string(have_ask ? aqty : 0);
  s += "}\n";
  return s;
}

/** @brief Render an L2 depth snapshot line (top 10 levels/side). */
std::string L2Json(const BookReplica& r, const Symbol& sym) {
  const auto emit = [](const std::vector<BookReplica::Level>& lv) {
    std::string s = "[";
    for (std::size_t i = 0; i < lv.size(); ++i) {
      if (i != 0) {
        s += ",";
      }
      s += "[" + std::to_string(lv[i].price) + "," +
           std::to_string(lv[i].qty) + "]";
    }
    s += "]";
    return s;
  };
  std::string s = "{\"t\":\"l2\",\"sym\":\"";
  s += sym;
  s += "\",\"bids\":";
  s += emit(r.depth(sym, Side::Buy, 10));
  s += ",\"asks\":";
  s += emit(r.depth(sym, Side::Sell, 10));
  s += "}\n";
  return s;
}

/** @brief Render an L3 (market-by-order) line for a price level. */
std::string L3Json(const BookReplica& r, const Symbol& sym, Side side,
                   Ticks price) {
  std::string s = "{\"t\":\"l3\",\"sym\":\"";
  s += sym;
  s += "\",\"side\":\"";
  s += side == Side::Buy ? "b" : "s";
  s += "\",\"price\":";
  s += std::to_string(price);
  s += ",\"orders\":[";
  const auto orders = r.orders_at(sym, side, price);
  for (std::size_t i = 0; i < orders.size(); ++i) {
    if (i != 0) {
      s += ",";
    }
    s += "[" + std::to_string(orders[i].id) + "," +
         std::to_string(orders[i].qty) + "]";
  }
  s += "]}\n";
  return s;
}

class FeedHelper;

/**
 * @brief One subscriber connection: a bounded outbound queue plus a line-based
 *        command reader, driven by the event loop.
 */
class Subscriber final : public IoHandler {
 public:
  Subscriber(EventLoop& loop, int fd, FeedHelper& helper)
      : loop_(loop), fd_(fd), helper_(helper) {}

  ~Subscriber() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  int fd() const { return fd_; }

  /** @brief Queue @p msg; disconnect self if it would overflow the cap. */
  void queue(const std::string& msg) {
    if (out_.size() + msg.size() > kSubscriberOutCap) {
      drop();  // slow consumer: drop it rather than stall the hot path
      return;
    }
    out_.append(msg);
    arm_write(true);
    flush();
  }

  void on_io_ready(int /*fd*/, IoEvents events) override;

 private:
  void on_readable();
  void handle_command(const std::string& line);
  void flush();
  void arm_write(bool on);
  void drop();

  EventLoop& loop_;
  int fd_;
  FeedHelper& helper_;
  Buffer in_;
  Buffer out_;
  bool write_armed_ = false;
  bool dropped_ = false;
};

/**
 * @brief Reads the book-event stream from stdin into the helper.
 */
class StdinReader final : public IoHandler {
 public:
  explicit StdinReader(FeedHelper& helper) : helper_(helper) {}
  void on_io_ready(int fd, IoEvents events) override;

 private:
  FeedHelper& helper_;
  Buffer in_;
};

/**
 * @brief The feed-helper: replica + subscriber registry + fan-out.
 */
class FeedHelper {
 public:
  explicit FeedHelper(EventLoop& loop) : loop_(loop), stdin_(*this) {}

  Status start(const std::string& addr, std::uint16_t port) {
    // stdin (fd 0) carries the event stream.
    const int flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    if (Status s = loop_.add(STDIN_FILENO, IoInterest::kRead, &stdin_);
        !s.ok()) {
      return s;
    }
    Result<std::unique_ptr<TcpListener>> lr = TcpListener::Create(
        loop_, addr, port, [this](int fd) { on_accept(fd); });
    if (!lr.ok()) {
      return lr.error();
    }
    listener_ = std::move(lr.value());
    std::printf("listening %u\n", listener_->port());
    std::fflush(stdout);
    return Status::Ok();
  }

  BookReplica& replica() { return replica_; }

  /** @brief Apply one decoded event and broadcast the symbol's new L1. */
  void on_event(const BookEvent& ev) {
    if (replica_.apply(ev)) {
      std::fprintf(stderr, "feed: seq gap (now %llu, %llu total)\n",
                   static_cast<unsigned long long>(replica_.last_seq()),
                   static_cast<unsigned long long>(replica_.gaps()));
    }
    broadcast(L1Json(replica_, ev.symbol));
  }

  /** @brief Send @p msg to every subscriber (dropping any that overflow). */
  void broadcast(const std::string& msg) {
    for (auto& [fd, sub] : subs_) {
      sub->queue(msg);
    }
  }

  /** @brief On stdin EOF the upstream is gone: stop serving. */
  void on_stdin_eof() { loop_.stop(); }

  /** @brief Remove a subscriber (deferred, so it is safe mid-dispatch). */
  void remove_subscriber(int fd) {
    loop_.remove(fd);
    loop_.defer([this, fd]() { subs_.erase(fd); });
  }

 private:
  void on_accept(int fd) {
    auto sub = std::make_unique<Subscriber>(loop_, fd, *this);
    if (Status s = loop_.add(fd, IoInterest::kRead, sub.get()); !s.ok()) {
      return;  // sub destructs, closing fd
    }
    // Greet with a snapshot: one L1 line per known symbol, then ready.
    // (A fuller protocol would let the subscriber pick symbols.)
    Subscriber& ref = *sub;
    subs_.emplace(fd, std::move(sub));
    for (const Symbol& sym : known_symbols()) {
      ref.queue(L1Json(replica_, sym));
    }
    ref.queue("{\"t\":\"ready\"}\n");
  }

  /** @return The symbols the replica currently knows (for the snapshot). */
  std::vector<Symbol> known_symbols() const {
    // BookReplica does not expose its symbol set; the snapshot is best-effort
    // over the symbols seen so far, tracked here as events arrive.
    return std::vector<Symbol>(seen_symbols_.begin(), seen_symbols_.end());
  }

 public:
  /** @brief Note a symbol so later subscribers get it in their snapshot. */
  void note_symbol(const Symbol& sym) { seen_symbols_.insert(sym); }

 private:
  EventLoop& loop_;
  StdinReader stdin_;
  std::unique_ptr<TcpListener> listener_;
  BookReplica replica_;
  std::unordered_map<int, std::unique_ptr<Subscriber>> subs_;
  std::set<Symbol> seen_symbols_;
};

void StdinReader::on_io_ready(int /*fd*/, IoEvents events) {
  // EOF (codicis closing the event pipe) surfaces as kReadable per the loop
  // contract on both backends, so the read loop below hits the 0-byte read.
  if (HasEvent(events, IoEvents::kReadable)) {
    for (;;) {
      std::uint8_t* dst = in_.reserve(kReadChunk);
      const ssize_t n = ::read(STDIN_FILENO, dst, kReadChunk);
      if (n > 0) {
        in_.commit(static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) {
        helper_.on_stdin_eof();
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      helper_.on_stdin_eof();
      return;
    }
    for (;;) {
      BookEvent ev;
      const FeedDecode d = DecodeBookEvent(in_, &ev);
      if (d == FeedDecode::kIncomplete) {
        break;
      }
      if (d == FeedDecode::kError) {
        helper_.on_stdin_eof();  // corrupt stream: stop
        return;
      }
      helper_.note_symbol(ev.symbol);
      helper_.on_event(ev);
    }
  }
}

void Subscriber::on_io_ready(int /*fd*/, IoEvents events) {
  if (HasEvent(events, IoEvents::kReadable)) {
    on_readable();  // a peer close surfaces as kReadable -> read()==0 -> drop
    if (dropped_) {
      return;
    }
  }
  if (HasEvent(events, IoEvents::kWritable)) {
    flush();
    if (dropped_) {
      return;
    }
  }
  if (HasEvent(events, IoEvents::kError)) {
    drop();
  }
}

void Subscriber::on_readable() {
  for (;;) {
    std::uint8_t* dst = in_.reserve(4096);
    const ssize_t n = ::read(fd_, dst, 4096);
    if (n > 0) {
      in_.commit(static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      drop();
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    drop();
    return;
  }
  // Process whole command lines.
  for (;;) {
    const std::string_view v = in_.view();
    const std::size_t nl = v.find('\n');
    if (nl == std::string_view::npos) {
      break;
    }
    std::string line(v.substr(0, nl));
    in_.consume(nl + 1);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    handle_command(line);
    if (dropped_) {
      return;
    }
  }
}

void Subscriber::flush() {
  while (!out_.empty()) {
    const ssize_t n = ::write(fd_, out_.data(), out_.size());
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
    drop();
    return;
  }
  if (out_.empty()) {
    arm_write(false);
  }
}

void Subscriber::arm_write(bool on) {
  if (on == write_armed_) {
    return;
  }
  write_armed_ = on;
  loop_.modify(fd_, on ? (IoInterest::kRead | IoInterest::kWrite)
                       : IoInterest::kRead);
}

void Subscriber::drop() {
  if (dropped_) {
    return;
  }
  dropped_ = true;
  helper_.remove_subscriber(fd_);
}

/**
 * @brief Handle one subscriber command line: l1/l2/l3 snapshot requests.
 *
 * Defined out of line, where FeedHelper (and its replica) is complete.
 */
void Subscriber::handle_command(const std::string& line) {
  // Tokenize on spaces.
  std::vector<std::string> tok;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') {
      ++i;
    }
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ') {
      ++i;
    }
    if (i > start) {
      tok.push_back(line.substr(start, i - start));
    }
  }
  if (tok.empty()) {
    return;
  }
  const BookReplica& r = helper_.replica();
  if (tok[0] == "l1" && tok.size() >= 2) {
    queue(L1Json(r, tok[1]));
  } else if (tok[0] == "l2" && tok.size() >= 2) {
    queue(L2Json(r, tok[1]));
  } else if (tok[0] == "l3" && tok.size() >= 4) {
    const Side side = tok[2] == "s" ? Side::Sell : Side::Buy;
    const Ticks px = static_cast<Ticks>(std::atoll(tok[3].c_str()));
    queue(L3Json(r, tok[1], side, px));
  }
  // Unknown commands are ignored.
}

}  // namespace
}  // namespace codicis

int main(int argc, char** argv) {
  using namespace codicis;

  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  if (argc > 2) {
    host = argv[1];
    port = static_cast<std::uint16_t>(std::atoi(argv[2]));
  } else if (const char* env = std::getenv("CODICIS_FEED_LISTEN")) {
    const std::string s = env;
    const std::size_t colon = s.find(':');
    if (colon != std::string::npos) {
      host = s.substr(0, colon);
      port = static_cast<std::uint16_t>(std::atoi(s.c_str() + colon + 1));
    }
  }

  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  if (!lr.ok()) {
    std::fprintf(stderr, "feed: event loop: %s\n",
                 lr.error().message.c_str());
    return 1;
  }
  EventLoop& loop = *lr.value();
  FeedHelper helper(loop);
  if (Status s = helper.start(host, port); !s.ok()) {
    std::fprintf(stderr, "feed: listen: %s\n", s.error().message.c_str());
    return 1;
  }
  loop.run();
  return 0;
}
