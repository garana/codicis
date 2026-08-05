/**
 * @file test_feed.cc
 * @brief Unit tests for the feed wire codec and the L1/L2/L3 book replica.
 */

#include "catch_amalgamated.hpp"

#include "codicis/core/book_event.h"
#include "codicis/event/event_loop.h"
#include "codicis/feed/book_replica.h"
#include "codicis/feed/feed_publisher.h"
#include "codicis/feed/feed_wire.h"
#include "codicis/util/buffer.h"
#include "codicis/util/clock.h"

#include <fcntl.h>
#include <unistd.h>

#include <memory>

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

}  // namespace

TEST_CASE("Feed wire round-trips a book event", "[feed][wire]") {
  BookEvent in = Add(7, "BTC", 42, Side::Sell, 12345, 99);
  in.taker_id = 5;
  in.prev_price = 12000;
  in.displayed = 40;

  std::string bytes;
  EncodeBookEvent(in, &bytes);

  Buffer buf;
  buf.append(bytes);
  BookEvent out;
  REQUIRE(DecodeBookEvent(buf, &out) == FeedDecode::kComplete);
  REQUIRE(buf.empty());
  REQUIRE(out.seq == 7);
  REQUIRE(out.symbol == "BTC");
  REQUIRE(out.type == BookEventType::kAdd);
  REQUIRE(out.order_id == 42);
  REQUIRE(out.taker_id == 5);
  REQUIRE(out.side == Side::Sell);
  REQUIRE(out.price == 12345);
  REQUIRE(out.prev_price == 12000);
  REQUIRE(out.qty == 99);
  REQUIRE(out.displayed == 40);
}

TEST_CASE("Feed wire is incremental and pipelined", "[feed][wire]") {
  std::string bytes;
  EncodeBookEvent(Add(1, "ETH", 1, Side::Buy, 100, 5), &bytes);
  EncodeBookEvent(Add(2, "ETH", 2, Side::Buy, 99, 7), &bytes);

  // A truncated first record decodes nothing.
  Buffer partial;
  partial.append(std::string_view(bytes.data(), 4));
  BookEvent tmp;
  REQUIRE(DecodeBookEvent(partial, &tmp) == FeedDecode::kIncomplete);
  REQUIRE(partial.size() == 4);  // nothing consumed

  // Both records back to back decode in order.
  Buffer buf;
  buf.append(bytes);
  BookEvent a;
  BookEvent b;
  REQUIRE(DecodeBookEvent(buf, &a) == FeedDecode::kComplete);
  REQUIRE(DecodeBookEvent(buf, &b) == FeedDecode::kComplete);
  REQUIRE(DecodeBookEvent(buf, &tmp) == FeedDecode::kIncomplete);
  REQUIRE(a.order_id == 1);
  REQUIRE(b.order_id == 2);
  REQUIRE(b.price == 99);
}

TEST_CASE("Replica builds L1/L2 from adds", "[feed][replica]") {
  BookReplica r;
  r.apply(Add(1, "BTC", 1, Side::Buy, 100, 5));
  r.apply(Add(2, "BTC", 2, Side::Buy, 101, 3));   // better bid
  r.apply(Add(3, "BTC", 3, Side::Buy, 100, 2));   // same level as order 1
  r.apply(Add(4, "BTC", 4, Side::Sell, 105, 8));
  r.apply(Add(5, "BTC", 5, Side::Sell, 106, 4));

  Ticks px = 0;
  Quantity qty = 0;
  REQUIRE(r.best_bid("BTC", &px, &qty));
  REQUIRE(px == 101);
  REQUIRE(qty == 3);
  REQUIRE(r.best_ask("BTC", &px, &qty));
  REQUIRE(px == 105);
  REQUIRE(qty == 8);

  // L2 bids: best (101) first, then 100 aggregating orders 1 + 3.
  const auto bids = r.depth("BTC", Side::Buy, 0);
  REQUIRE(bids.size() == 2);
  REQUIRE(bids[0].price == 101);
  REQUIRE(bids[0].qty == 3);
  REQUIRE(bids[1].price == 100);
  REQUIRE(bids[1].qty == 7);  // 5 + 2 aggregated

  // L3 at 100 lists both orders.
  const auto l3 = r.orders_at("BTC", Side::Buy, 100);
  REQUIRE(l3.size() == 2);
}

TEST_CASE("Replica applies cancel, trade and reprice", "[feed][replica]") {
  BookReplica r;
  r.apply(Add(1, "BTC", 1, Side::Sell, 105, 10));
  r.apply(Add(2, "BTC", 2, Side::Sell, 105, 4));

  Ticks px = 0;
  Quantity qty = 0;
  REQUIRE(r.best_ask("BTC", &px, &qty));
  REQUIRE(qty == 14);

  SECTION("a trade shrinks the maker and its level") {
    BookEvent t;
    t.seq = 3;
    t.symbol = "BTC";
    t.type = BookEventType::kTrade;
    t.order_id = 1;  // maker
    t.taker_id = 99;
    t.side = Side::Buy;  // aggressor
    t.price = 105;
    t.qty = 6;
    r.apply(t);
    REQUIRE(r.best_ask("BTC", &px, &qty));
    REQUIRE(qty == 8);  // 14 - 6
    const auto l3 = r.orders_at("BTC", Side::Sell, 105);
    REQUIRE(l3.size() == 2);  // order 1 has 4 left, order 2 has 4
  }

  SECTION("a cancel removes an order from its level") {
    BookEvent c;
    c.seq = 3;
    c.symbol = "BTC";
    c.type = BookEventType::kCancel;
    c.order_id = 2;
    r.apply(c);
    REQUIRE(r.best_ask("BTC", &px, &qty));
    REQUIRE(qty == 10);
    REQUIRE(r.orders_at("BTC", Side::Sell, 105).size() == 1);
  }

  SECTION("a reprice moves an order to a new level") {
    BookEvent rp;
    rp.seq = 3;
    rp.symbol = "BTC";
    rp.type = BookEventType::kReprice;
    rp.order_id = 2;
    rp.side = Side::Sell;
    rp.prev_price = 105;
    rp.price = 107;
    rp.qty = 4;
    r.apply(rp);
    REQUIRE(r.best_ask("BTC", &px, &qty));
    REQUIRE(px == 105);
    REQUIRE(qty == 10);  // order 1 only
    const auto asks = r.depth("BTC", Side::Sell, 0);
    REQUIRE(asks.size() == 2);
    REQUIRE(asks[1].price == 107);
    REQUIRE(asks[1].qty == 4);
  }
}

TEST_CASE("Replica separates matchable from displayed depth",
          "[feed][replica]") {
  BookReplica r;
  // A normal bid: displayed == matchable.
  BookEvent normal = Add(1, "BTC", 1, Side::Buy, 100, 5);
  normal.displayed = 5;
  r.apply(normal);
  // A hidden bid at a BETTER price: matchable-only, contributes 0 lit.
  BookEvent hidden = Add(2, "BTC", 2, Side::Buy, 101, 8);
  hidden.displayed = 0;
  r.apply(hidden);
  // An iceberg bid at 100: 10 matchable, only a 3-slice lit.
  BookEvent ice = Add(3, "BTC", 3, Side::Buy, 100, 10);
  ice.displayed = 3;
  r.apply(ice);

  Ticks px = 0;
  Quantity qty = 0;
  // Matchable L1: the hidden order at 101 is the best matchable bid.
  REQUIRE(r.best_bid("BTC", &px, &qty));
  REQUIRE(px == 101);
  REQUIRE(qty == 8);
  // Displayed L1: the hidden order is invisible, so the lit best bid is 100
  // (normal 5 + iceberg slice 3 = 8 lit).
  REQUIRE(r.best_displayed_bid("BTC", &px, &qty));
  REQUIRE(px == 100);
  REQUIRE(qty == 8);

  // Matchable L2 shows 101 (8) then 100 (15 = 5 + 10). Displayed L2 omits 101
  // entirely and shows 100 with 8 lit.
  const auto full = r.depth("BTC", Side::Buy, 0);
  REQUIRE(full.size() == 2);
  REQUIRE(full[0].price == 101);
  REQUIRE(full[0].qty == 8);
  REQUIRE(full[1].price == 100);
  REQUIRE(full[1].qty == 15);

  const auto lit = r.displayed_depth("BTC", Side::Buy, 0);
  REQUIRE(lit.size() == 1);  // 101 is hidden-only -> absent from the lit book
  REQUIRE(lit[0].price == 100);
  REQUIRE(lit[0].qty == 8);
}

TEST_CASE("Replica detects a sequence gap", "[feed][replica]") {
  BookReplica r;
  REQUIRE_FALSE(r.apply(Add(1, "BTC", 1, Side::Buy, 100, 5)));
  REQUIRE_FALSE(r.apply(Add(2, "BTC", 2, Side::Buy, 100, 5)));
  REQUIRE(r.apply(Add(5, "BTC", 3, Side::Buy, 100, 5)));  // 3,4 lost -> gap
  REQUIRE(r.gaps() == 1);
  REQUIRE(r.last_seq() == 5);
}

TEST_CASE("FeedPublisher writes the encoded event stream", "[feed][publisher]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  int p[2];
  REQUIRE(::pipe(p) == 0);
  const int flags = ::fcntl(p[1], F_GETFL, 0);
  ::fcntl(p[1], F_SETFL, flags | O_NONBLOCK);

  // The publisher owns and drains the write end onto the loop; the read end
  // stands in for the feed-helper's stdin.
  FeedPublisher pub(loop, p[1]);
  pub.on_book_event(Add(1, "BTC", 1, Side::Sell, 105, 8));
  pub.on_book_event(Add(2, "BTC", 2, Side::Buy, 101, 3));
  for (int i = 0; i < 10; ++i) {
    loop.run_once(5);
  }

  char raw[512];
  const ssize_t n = ::read(p[0], raw, sizeof(raw));
  REQUIRE(n > 0);
  Buffer buf;
  buf.append(std::string_view(raw, static_cast<std::size_t>(n)));

  BookEvent a;
  BookEvent b;
  REQUIRE(DecodeBookEvent(buf, &a) == FeedDecode::kComplete);
  REQUIRE(DecodeBookEvent(buf, &b) == FeedDecode::kComplete);
  REQUIRE(a.order_id == 1);
  REQUIRE(a.price == 105);
  REQUIRE(b.order_id == 2);
  REQUIRE(b.side == Side::Buy);
  REQUIRE(pub.dropped() == 0);

  ::close(p[0]);  // pub closes p[1] in its destructor
}
