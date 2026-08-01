/**
 * @file test_order_book.cc
 * @brief Unit tests for order normalization and the matching engine.
 */

#include "catch_amalgamated.hpp"

#include "codicis/core/order.h"
#include "codicis/core/order_book.h"

using namespace codicis;

namespace {

/** @brief Build a limit order. */
Order Limit(OrderId id, Side side, Ticks price, Quantity qty,
            Tif tif = Tif::GTC, std::uint32_t flags = 0) {
  Order o;
  o.id = id;
  o.side = side;
  o.type = OrdType::Limit;
  o.tif = tif;
  o.flags = flags;
  o.price = price;
  o.qty = qty;
  return o;
}

/** @brief Build a market order. */
Order Market(OrderId id, Side side, Quantity qty) {
  Order o;
  o.id = id;
  o.side = side;
  o.type = OrdType::Market;
  o.tif = Tif::IOC;
  o.qty = qty;
  return o;
}

/** @brief Build a stop-market order (becomes a market order when triggered). */
Order StopMarket(OrderId id, Side side, Ticks stop, Quantity qty) {
  Order o = Market(id, side, qty);
  TriggerSpec t;
  t.kind = TriggerSpec::Kind::Stop;
  t.stop_price = stop;
  o.trigger = t;
  return o;
}

/** @brief Build a stop-limit order (becomes a limit order when triggered). */
Order StopLimit(OrderId id, Side side, Ticks stop, Ticks limit, Quantity qty) {
  Order o = Limit(id, side, limit, qty);
  TriggerSpec t;
  t.kind = TriggerSpec::Kind::Stop;
  t.stop_price = stop;
  o.trigger = t;
  return o;
}

/** @brief Build a trailing-stop-market order with a fixed trail amount. */
Order TrailingStop(OrderId id, Side side, Ticks trail, Quantity qty) {
  Order o = Market(id, side, qty);
  TriggerSpec t;
  t.kind = TriggerSpec::Kind::TrailAmount;
  t.trail_by = trail;
  o.trigger = t;
  return o;
}

/** @brief Build an iceberg limit order with a visible slice. */
Order Iceberg(OrderId id, Side side, Ticks price, Quantity qty,
              Quantity display) {
  Order o = Limit(id, side, price, qty);
  SetFlag(&o.flags, OrderFlag::kIceberg);
  o.display_qty = display;
  return o;
}

/** @brief Build a fully hidden limit order. */
Order HiddenLimit(OrderId id, Side side, Ticks price, Quantity qty) {
  Order o = Limit(id, side, price, qty);
  SetFlag(&o.flags, OrderFlag::kHidden);
  return o;
}

/** @brief Force a trade at @p px so the book has a last price; leaves it empty. */
void SeedLast(OrderBook& book, Ticks px, OrderId* next_id) {
  book.submit(Limit((*next_id)++, Side::Sell, px, 1));
  book.submit(Limit((*next_id)++, Side::Buy, px, 1));
}

/** @brief Build a pegged order. */
Order Peg(OrderId id, Side side, PegSpec::Ref ref, Ticks offset, Quantity qty,
          Ticks cap = 0) {
  Order o;
  o.id = id;
  o.side = side;
  o.type = OrdType::Limit;
  o.tif = Tif::GTC;
  o.qty = qty;
  PegSpec p;
  p.ref = ref;
  p.offset = offset;
  p.cap_limit = cap;
  o.peg = p;
  return o;
}

/** @return True if any trade in the outcome has taker @p id. */
bool HasTaker(const SubmitOutcome& out, OrderId id) {
  for (const Trade& t : out.trades) {
    if (t.taker_id == id) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("Normalize collapses convenience TIFs", "[core][order]") {
  Order gtx = Limit(1, Side::Buy, 100, 10, Tif::GTX);
  Normalize(&gtx);
  REQUIRE(gtx.tif == Tif::GTC);
  REQUIRE(HasFlag(gtx.flags, OrderFlag::kPostOnly));
  REQUIRE(gtx.leaves == 10);
  REQUIRE(gtx.display_qty == 10);

  Order fok = Limit(2, Side::Sell, 100, 10, Tif::FOK);
  Normalize(&fok);
  REQUIRE(fok.tif == Tif::IOC);
  REQUIRE(HasFlag(fok.flags, OrderFlag::kAon));
}

TEST_CASE("Resting limit orders set the top of book", "[core][book]") {
  OrderBook book;
  book.submit(Limit(1, Side::Buy, 99, 10));
  book.submit(Limit(2, Side::Sell, 101, 10));

  Ticks bid = 0;
  Ticks ask = 0;
  REQUIRE(book.best_bid(&bid));
  REQUIRE(book.best_ask(&ask));
  REQUIRE(bid == 99);
  REQUIRE(ask == 101);
  REQUIRE(book.resting_count() == 2);
}

TEST_CASE("A crossing order trades at the maker price", "[core][match]") {
  OrderBook book;
  book.submit(Limit(1, Side::Sell, 100, 10));   // resting maker
  SubmitOutcome out = book.submit(Limit(2, Side::Buy, 105, 10));  // aggressor

  REQUIRE(out.accepted);
  REQUIRE(out.trades.size() == 1);
  REQUIRE(out.trades[0].price == 100);  // price improvement to the taker
  REQUIRE(out.trades[0].qty == 10);
  REQUIRE(out.trades[0].maker_id == 1);
  REQUIRE(out.trades[0].taker_id == 2);
  REQUIRE(out.filled == 10);
  REQUIRE_FALSE(out.rested);
  REQUIRE(book.resting_count() == 0);
}

TEST_CASE("Partial fill rests the remainder", "[core][match]") {
  OrderBook book;
  book.submit(Limit(1, Side::Sell, 100, 4));
  SubmitOutcome out = book.submit(Limit(2, Side::Buy, 100, 10));
  REQUIRE(out.filled == 4);
  REQUIRE(out.rested);
  Ticks bid = 0;
  REQUIRE(book.best_bid(&bid));
  REQUIRE(bid == 100);
  REQUIRE(book.total_qty_at(Side::Buy, 100) == 6);
}

TEST_CASE("Matching preserves FIFO time priority", "[core][match]") {
  OrderBook book;
  book.submit(Limit(1, Side::Sell, 100, 5));  // first in queue
  book.submit(Limit(2, Side::Sell, 100, 5));  // second
  SubmitOutcome out = book.submit(Limit(3, Side::Buy, 100, 5));
  REQUIRE(out.trades.size() == 1);
  REQUIRE(out.trades[0].maker_id == 1);  // oldest filled first
  REQUIRE(book.find(2) != nullptr);      // second still rests
}

TEST_CASE("An aggressor sweeps multiple price levels", "[core][match]") {
  OrderBook book;
  book.submit(Limit(1, Side::Sell, 100, 5));
  book.submit(Limit(2, Side::Sell, 101, 5));
  SubmitOutcome out = book.submit(Limit(3, Side::Buy, 101, 8));
  REQUIRE(out.trades.size() == 2);
  REQUIRE(out.trades[0].price == 100);
  REQUIRE(out.trades[0].qty == 5);
  REQUIRE(out.trades[1].price == 101);
  REQUIRE(out.trades[1].qty == 3);
  REQUIRE(out.filled == 8);
}

TEST_CASE("IOC discards the unfilled remainder", "[core][tif]") {
  OrderBook book;
  book.submit(Limit(1, Side::Sell, 100, 5));
  SubmitOutcome out = book.submit(Limit(2, Side::Buy, 100, 12, Tif::IOC));
  REQUIRE(out.filled == 5);
  REQUIRE_FALSE(out.rested);
  REQUIRE(book.resting_count() == 0);
}

TEST_CASE("FOK is all-or-none", "[core][tif]") {
  SECTION("rejected when not fully fillable") {
    OrderBook book;
    book.submit(Limit(1, Side::Sell, 100, 5));
    SubmitOutcome out = book.submit(Limit(2, Side::Buy, 100, 12, Tif::FOK));
    REQUIRE_FALSE(out.accepted);
    REQUIRE(out.trades.empty());
    REQUIRE(book.total_qty_at(Side::Sell, 100) == 5);  // maker untouched
  }
  SECTION("filled when fully fillable") {
    OrderBook book;
    book.submit(Limit(1, Side::Sell, 100, 5));
    book.submit(Limit(2, Side::Sell, 101, 10));
    SubmitOutcome out = book.submit(Limit(3, Side::Buy, 101, 12, Tif::FOK));
    REQUIRE(out.accepted);
    REQUIRE(out.filled == 12);
  }
}

TEST_CASE("Post-only rejects when it would cross", "[core][flags]") {
  OrderBook book;
  book.submit(Limit(1, Side::Sell, 100, 5));
  SECTION("crossing is rejected") {
    SubmitOutcome out = book.submit(Limit(2, Side::Buy, 100, 5, Tif::GTX));
    REQUIRE_FALSE(out.accepted);
    REQUIRE(book.resting_count() == 1);  // only the maker
  }
  SECTION("non-crossing rests as maker") {
    SubmitOutcome out = book.submit(Limit(3, Side::Buy, 99, 5, Tif::GTX));
    REQUIRE(out.accepted);
    REQUIRE(out.rested);
    Ticks bid = 0;
    REQUIRE(book.best_bid(&bid));
    REQUIRE(bid == 99);
  }
}

TEST_CASE("Market order sweeps and discards the remainder", "[core][match]") {
  OrderBook book;
  book.submit(Limit(1, Side::Sell, 100, 5));
  book.submit(Limit(2, Side::Sell, 101, 5));
  SubmitOutcome out = book.submit(Market(3, Side::Buy, 7));
  REQUIRE(out.filled == 7);
  REQUIRE_FALSE(out.rested);
  REQUIRE(book.total_qty_at(Side::Sell, 101) == 3);
}

TEST_CASE("Self-trade prevention: cancel resting", "[core][stp]") {
  OrderBook book(StpPolicy::kCancelResting);
  Order maker = Limit(1, Side::Sell, 100, 5);
  maker.client_id = 5;
  book.submit(maker);
  Order taker = Limit(2, Side::Buy, 100, 5);
  taker.client_id = 5;
  SubmitOutcome out = book.submit(taker);
  REQUIRE(out.trades.empty());        // no self-trade
  REQUIRE(book.find(1) == nullptr);   // resting maker cancelled
  REQUIRE(out.rested);                // taker rests instead
  Ticks bid = 0;
  REQUIRE(book.best_bid(&bid));
  REQUIRE(bid == 100);
}

TEST_CASE("Self-trade prevention: cancel aggressor", "[core][stp]") {
  OrderBook book(StpPolicy::kCancelAggressor);
  Order maker = Limit(1, Side::Sell, 100, 5);
  maker.client_id = 5;
  book.submit(maker);
  Order taker = Limit(2, Side::Buy, 100, 5);
  taker.client_id = 5;
  SubmitOutcome out = book.submit(taker);
  REQUIRE(out.trades.empty());
  REQUIRE_FALSE(out.rested);          // aggressor remainder cancelled
  REQUIRE(book.find(1) != nullptr);   // maker untouched
  REQUIRE(book.resting_count() == 1);
}

TEST_CASE("Self-trade prevention does not affect other accounts",
          "[core][stp]") {
  OrderBook book(StpPolicy::kCancelResting);
  Order maker = Limit(1, Side::Sell, 100, 5);
  maker.client_id = 5;
  book.submit(maker);
  Order taker = Limit(2, Side::Buy, 100, 5);
  taker.client_id = 9;  // different account
  SubmitOutcome out = book.submit(taker);
  REQUIRE(out.trades.size() == 1);
  REQUIRE(out.filled == 5);
}

TEST_CASE("Min-quantity requires a floor to be available", "[core][flags]") {
  SECTION("rejected when floor not available") {
    OrderBook book;
    book.submit(Limit(1, Side::Sell, 100, 5));
    Order o = Limit(2, Side::Buy, 100, 20);
    o.min_qty = 10;
    SubmitOutcome out = book.submit(o);
    REQUIRE_FALSE(out.accepted);
    REQUIRE(book.total_qty_at(Side::Sell, 100) == 5);
  }
  SECTION("accepted with partials above the floor") {
    OrderBook book;
    book.submit(Limit(1, Side::Sell, 100, 5));
    Order o = Limit(2, Side::Buy, 100, 20);
    o.min_qty = 5;
    SubmitOutcome out = book.submit(o);
    REQUIRE(out.accepted);
    REQUIRE(out.filled == 5);
  }
}

TEST_CASE("Expiry cancels timed-out resting orders", "[core][tif]") {
  OrderBook book;
  Order a = Limit(1, Side::Buy, 99, 10);
  a.expiry_ns = 1000;
  book.submit(a);
  Order b = Limit(2, Side::Buy, 98, 10);  // no expiry
  book.submit(b);

  std::vector<OrderId> expired = book.expire(500);
  REQUIRE(expired.empty());  // not yet due

  expired = book.expire(1000);
  REQUIRE(expired.size() == 1);
  REQUIRE(expired[0] == 1);
  REQUIRE(book.find(1) == nullptr);
  REQUIRE(book.find(2) != nullptr);
}

TEST_CASE("Stop-market parks then fires when price crosses the stop",
          "[core][stop]") {
  OrderBook book;
  OrderId id = 1;
  SeedLast(book, 100, &id);          // last = 100, book empty
  book.submit(Limit(id++, Side::Sell, 106, 10));  // liquidity for the stop
  book.submit(Limit(id++, Side::Sell, 110, 10));

  const OrderId stop_id = id++;
  SubmitOutcome parked = book.submit(StopMarket(stop_id, Side::Buy, 105, 5));
  REQUIRE(parked.accepted);
  REQUIRE(parked.pending_trigger);
  REQUIRE(book.pending_stop_count() == 1);

  // A trade at 106 pushes the last price past the 105 stop.
  const OrderId trig = id++;
  SubmitOutcome out = book.submit(Limit(trig, Side::Buy, 106, 1));
  REQUIRE(book.pending_stop_count() == 0);
  REQUIRE(HasTaker(out, stop_id));           // the stop executed in this cascade
  REQUIRE(book.total_qty_at(Side::Sell, 106) == 4);  // 10 - 1 - 5
}

TEST_CASE("Triggered stop-limit can rest instead of trading", "[core][stop]") {
  OrderBook book;
  OrderId id = 1;
  SeedLast(book, 100, &id);
  book.submit(Limit(id++, Side::Sell, 106, 10));

  // Buy stop-limit: trigger 105, limit 105 (won't cross the 106 ask).
  const OrderId stop_id = id++;
  book.submit(StopLimit(stop_id, Side::Buy, 105, 105, 5));
  REQUIRE(book.pending_stop_count() == 1);

  book.submit(Limit(id++, Side::Buy, 106, 1));  // trade at 106 -> triggers
  REQUIRE(book.pending_stop_count() == 0);
  REQUIRE(book.find(stop_id) != nullptr);       // rested as a limit
  Ticks bid = 0;
  REQUIRE(book.best_bid(&bid));
  REQUIRE(bid == 105);
}

TEST_CASE("Trailing stop re-anchors on favorable moves", "[core][stop]") {
  OrderBook book;
  OrderId id = 1;
  SeedLast(book, 100, &id);
  book.submit(Limit(id++, Side::Buy, 80, 20));  // deep bid for the exit

  // Sell trailing stop, trail 5: armed stop = 100 - 5 = 95.
  const OrderId stop_id = id++;
  book.submit(TrailingStop(stop_id, Side::Sell, 5, 5));
  REQUIRE(book.pending_stop_count() == 1);

  SeedLast(book, 110, &id);  // peak rises -> stop trails up to 105
  REQUIRE(book.pending_stop_count() == 1);

  SeedLast(book, 106, &id);  // above the trailed stop (105): no trigger
  REQUIRE(book.pending_stop_count() == 1);

  SeedLast(book, 105, &id);  // touches the trailed stop -> fires
  REQUIRE(book.pending_stop_count() == 0);
  Ticks last = 0;
  REQUIRE(book.last_trade_price(&last));
}

TEST_CASE("A triggered stop can cascade into another", "[core][stop]") {
  OrderBook book;
  OrderId id = 1;
  SeedLast(book, 100, &id);
  book.submit(Limit(id++, Side::Sell, 106, 5));
  book.submit(Limit(id++, Side::Sell, 108, 5));

  const OrderId s1 = id++;
  const OrderId s2 = id++;
  book.submit(StopMarket(s1, Side::Buy, 105, 5));
  book.submit(StopMarket(s2, Side::Buy, 107, 5));
  REQUIRE(book.pending_stop_count() == 2);

  // One trade at 106 fires s1, whose market buy walks price to 108, firing s2.
  SubmitOutcome out = book.submit(Limit(id++, Side::Buy, 106, 1));
  REQUIRE(book.pending_stop_count() == 0);
  REQUIRE(HasTaker(out, s1));
  REQUIRE(HasTaker(out, s2));
}

TEST_CASE("Iceberg shows only its slice but matches its full reserve",
          "[core][iceberg]") {
  OrderBook book;
  book.submit(Iceberg(1, Side::Buy, 100, 10, 3));
  REQUIRE(book.total_qty_at(Side::Buy, 100) == 10);
  REQUIRE(book.displayed_qty_at(Side::Buy, 100) == 3);
}

TEST_CASE("Iceberg replenishes and loses priority to the queue",
          "[core][iceberg]") {
  OrderBook book;
  book.submit(Iceberg(1, Side::Buy, 100, 10, 3));  // slice 3, reserve 10
  book.submit(Limit(2, Side::Buy, 100, 5));        // queued behind

  // Sell 9: takes the iceberg's 3-slice, which then re-queues behind order 2,
  // so the next fills go to order 2 before returning to the iceberg.
  SubmitOutcome out = book.submit(Limit(3, Side::Sell, 100, 9));
  REQUIRE(out.trades.size() == 3);
  REQUIRE(out.trades[0].maker_id == 1);
  REQUIRE(out.trades[0].qty == 3);
  REQUIRE(out.trades[1].maker_id == 2);  // priority passed to the queued order
  REQUIRE(out.trades[1].qty == 5);
  REQUIRE(out.trades[2].maker_id == 1);  // iceberg's refreshed slice
  REQUIRE(out.trades[2].qty == 1);

  REQUIRE(book.total_qty_at(Side::Buy, 100) == 6);      // iceberg reserve left
  REQUIRE(book.displayed_qty_at(Side::Buy, 100) == 2);  // its current slice
}

TEST_CASE("Hidden orders match but are not displayed", "[core][hidden]") {
  OrderBook book;
  book.submit(HiddenLimit(1, Side::Buy, 100, 10));
  REQUIRE(book.total_qty_at(Side::Buy, 100) == 10);
  REQUIRE(book.displayed_qty_at(Side::Buy, 100) == 0);

  SubmitOutcome out = book.submit(Limit(2, Side::Sell, 100, 4));
  REQUIRE(out.trades.size() == 1);
  REQUIRE(out.trades[0].qty == 4);
  REQUIRE(book.total_qty_at(Side::Buy, 100) == 6);
  REQUIRE(book.displayed_qty_at(Side::Buy, 100) == 0);
}

TEST_CASE("Midpoint peg sits at the midpoint and reprices on BBO moves",
          "[core][peg]") {
  OrderBook book;
  book.submit(Limit(1, Side::Buy, 100, 5));
  book.submit(Limit(2, Side::Sell, 110, 5));

  book.submit(Peg(3, Side::Buy, PegSpec::Ref::Midpoint, 0, 5));
  REQUIRE(book.find(3) != nullptr);
  REQUIRE(book.find(3)->price == 105);  // (100 + 110) / 2

  book.submit(Limit(4, Side::Buy, 104, 5));  // best non-peg bid -> 104
  REQUIRE(book.find(3)->price == 107);       // (104 + 110) / 2
  REQUIRE(book.total_qty_at(Side::Buy, 105) == 0);
  REQUIRE(book.total_qty_at(Side::Buy, 107) == 5);
}

TEST_CASE("Primary peg tracks the same-side best", "[core][peg]") {
  OrderBook book;
  book.submit(Limit(1, Side::Buy, 100, 5));
  book.submit(Limit(2, Side::Sell, 110, 5));

  book.submit(Peg(3, Side::Buy, PegSpec::Ref::Primary, 0, 5));
  REQUIRE(book.find(3)->price == 100);            // joins the best bid
  REQUIRE(book.total_qty_at(Side::Buy, 100) == 10);

  book.submit(Limit(4, Side::Buy, 103, 5));       // best non-peg bid -> 103
  REQUIRE(book.find(3)->price == 103);
  REQUIRE(book.total_qty_at(Side::Buy, 100) == 5);   // only order 1 left here
  REQUIRE(book.total_qty_at(Side::Buy, 103) == 10);  // order 4 + peg
}

TEST_CASE("Market peg references the opposite side, with an offset",
          "[core][peg]") {
  OrderBook book;
  book.submit(Limit(1, Side::Buy, 100, 5));
  book.submit(Limit(2, Side::Sell, 110, 5));

  // Sell market peg references the best bid (100) + 8 = 108 (passive).
  book.submit(Peg(3, Side::Sell, PegSpec::Ref::Market, 8, 5));
  REQUIRE(book.find(3)->price == 108);

  book.submit(Limit(4, Side::Buy, 102, 5));  // best bid -> 102
  REQUIRE(book.find(3)->price == 110);       // 102 + 8
}

TEST_CASE("Peg cap limits the price", "[core][peg]") {
  OrderBook book;
  book.submit(Limit(1, Side::Buy, 100, 5));
  book.submit(Limit(2, Side::Sell, 110, 5));
  // Midpoint would be 105, but the buy cap of 103 clamps it.
  book.submit(Peg(3, Side::Buy, PegSpec::Ref::Midpoint, 0, 5, /*cap=*/103));
  REQUIRE(book.find(3)->price == 103);
}

TEST_CASE("Peg with no reference is rejected", "[core][peg]") {
  OrderBook book;
  book.submit(Limit(1, Side::Buy, 100, 5));  // no ask -> no midpoint reference
  SubmitOutcome out = book.submit(Peg(2, Side::Buy, PegSpec::Ref::Midpoint, 0,
                                      5));
  REQUIRE_FALSE(out.accepted);
}

TEST_CASE("A resting peg is matchable when crossed", "[core][peg]") {
  OrderBook book;
  book.submit(Limit(1, Side::Buy, 100, 5));
  book.submit(Limit(2, Side::Sell, 110, 5));
  book.submit(Peg(3, Side::Buy, PegSpec::Ref::Midpoint, 0, 5));  // bid @ 105

  SubmitOutcome out = book.submit(Limit(4, Side::Sell, 105, 3));  // hits the peg
  REQUIRE(out.trades.size() == 1);
  REQUIRE(out.trades[0].maker_id == 3);
  REQUIRE(out.trades[0].price == 105);
  REQUIRE(book.total_qty_at(Side::Buy, 105) == 2);  // peg remainder
}

TEST_CASE("A parked stop can be cancelled before triggering", "[core][stop]") {
  OrderBook book;
  OrderId id = 1;
  SeedLast(book, 100, &id);
  const OrderId stop_id = id++;
  book.submit(StopMarket(stop_id, Side::Buy, 105, 5));
  REQUIRE(book.pending_stop_count() == 1);
  REQUIRE(book.cancel(stop_id));
  REQUIRE(book.pending_stop_count() == 0);
  REQUIRE_FALSE(book.cancel(stop_id));
}

TEST_CASE("Cancel removes a resting order and updates the top",
          "[core][cancel]") {
  OrderBook book;
  book.submit(Limit(1, Side::Buy, 99, 10));
  book.submit(Limit(2, Side::Buy, 98, 10));
  REQUIRE(book.cancel(1));
  Ticks bid = 0;
  REQUIRE(book.best_bid(&bid));
  REQUIRE(bid == 98);  // next level becomes best
  REQUIRE_FALSE(book.cancel(1));  // already gone
  REQUIRE(book.find(1) == nullptr);
}
