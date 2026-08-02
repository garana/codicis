/**
 * @file test_matrix.cc
 * @brief Cross-product ("N x N") tests: each maker order type hit by each
 *        aggressor order type.
 *
 * The uniform grid rests one maker (total 10) on the ask at price 100 and hits
 * it with each aggressor kind, asserting fill quantity and residual. Two grids
 * cover aggressor-smaller-than-maker (partial) and aggressor-larger (which
 * separates the all-or-none aggressors from the rest). Pegged makers and a few
 * edge combinations that need special setup are tested separately.
 */

#include "catch_amalgamated.hpp"

#include "codicis/core/order.h"
#include "codicis/core/order_book.h"

#include <functional>
#include <string>
#include <vector>

using namespace codicis;

namespace {

constexpr Ticks kMakerPrice = 100;
constexpr Ticks kCrossPrice = 105;   // marketable buy limit
constexpr Quantity kMakerQty = 10;

Order MakeLimit(OrderId id, Side side, Ticks price, Quantity qty,
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

Order MakeMarket(OrderId id, Side side, Quantity qty) {
  Order o;
  o.id = id;
  o.side = side;
  o.type = OrdType::Market;
  o.tif = Tif::IOC;
  o.qty = qty;
  return o;
}

Order MakeIceberg(OrderId id, Side side, Ticks price, Quantity qty,
                  Quantity display = 3) {
  Order o = MakeLimit(id, side, price, qty);
  SetFlag(&o.flags, OrderFlag::kIceberg);
  o.display_qty = display;
  return o;
}

Order MakeHidden(OrderId id, Side side, Ticks price, Quantity qty) {
  Order o = MakeLimit(id, side, price, qty);
  SetFlag(&o.flags, OrderFlag::kHidden);
  return o;
}

/** @brief A midpoint-pegged order (price is derived from the reference BBO). */
Order MakeMidpointPeg(OrderId id, Side side, Quantity qty) {
  Order o;
  o.id = id;
  o.side = side;
  o.type = OrdType::Limit;
  o.tif = Tif::GTC;
  o.qty = qty;
  PegSpec p;
  p.ref = PegSpec::Ref::Midpoint;
  o.peg = p;
  return o;
}

/** @brief A primary-pegged order (pegs to the same-side best price). */
Order MakePrimaryPeg(OrderId id, Side side, Quantity qty) {
  Order o;
  o.id = id;
  o.side = side;
  o.type = OrdType::Limit;
  o.tif = Tif::GTC;
  o.qty = qty;
  PegSpec p;
  p.ref = PegSpec::Ref::Primary;
  o.peg = p;
  return o;
}

/** @brief A stop-market order that becomes a market order when triggered. */
Order MakeStopMarket(OrderId id, Side side, Ticks stop, Quantity qty) {
  Order o = MakeMarket(id, side, qty);
  TriggerSpec t;
  t.kind = TriggerSpec::Kind::Stop;
  t.stop_price = stop;
  o.trigger = t;
  return o;
}

/** @return True if a trade with the given taker and maker exists. */
bool HasTakerMaker(const SubmitOutcome& out, OrderId taker, OrderId maker) {
  for (const Trade& t : out.trades) {
    if (t.taker_id == taker && t.maker_id == maker) {
      return true;
    }
  }
  return false;
}

/** @brief A resting maker kind, buildable at any price/quantity. */
struct Maker {
  std::string name;
  std::function<Order(OrderId, Ticks, Quantity)> build;
};

/** @brief An aggressor kind (a crossing buy of a given quantity). */
struct Aggressor {
  std::string name;
  bool all_or_none;  // never partially fills
  bool immediate;    // IOC-like: rejects rather than resting when unfilled
  std::function<Order(OrderId, Quantity)> build;
};

std::vector<Maker> Makers() {
  return {
      {"limit",
       [](OrderId id, Ticks px, Quantity q) {
         return MakeLimit(id, Side::Sell, px, q);
       }},
      {"iceberg",
       [](OrderId id, Ticks px, Quantity q) {
         return MakeIceberg(id, Side::Sell, px, q);
       }},
      {"hidden",
       [](OrderId id, Ticks px, Quantity q) {
         return MakeHidden(id, Side::Sell, px, q);
       }},
  };
}

std::vector<Aggressor> Aggressors() {
  return {
      {"limit", false, false,
       [](OrderId id, Quantity q) {
         return MakeLimit(id, Side::Buy, kCrossPrice, q);
       }},
      {"market", false, true,
       [](OrderId id, Quantity q) { return MakeMarket(id, Side::Buy, q); }},
      {"ioc", false, true,
       [](OrderId id, Quantity q) {
         return MakeLimit(id, Side::Buy, kCrossPrice, q, Tif::IOC);
       }},
      {"fok", true, true,  // IOC + AON: rejects when it cannot fully fill
       [](OrderId id, Quantity q) {
         return MakeLimit(id, Side::Buy, kCrossPrice, q, Tif::FOK);
       }},
      {"aon", true, false,  // GTC + AON: rests when it cannot fully fill
       [](OrderId id, Quantity q) {
         Order o = MakeLimit(id, Side::Buy, kCrossPrice, q);
         SetFlag(&o.flags, OrderFlag::kAon);
         return o;
       }},
      {"minqty", false, false,
       [](OrderId id, Quantity q) {
         Order o = MakeLimit(id, Side::Buy, kCrossPrice, q);
         o.min_qty = 2;
         return o;
       }},
  };
}

Quantity TradedQty(const SubmitOutcome& out) {
  Quantity total = 0;
  for (const Trade& t : out.trades) {
    total += t.qty;
  }
  return total;
}

}  // namespace

TEST_CASE("Matrix: aggressor smaller than maker fills partially", "[matrix]") {
  for (const Maker& maker : Makers()) {
    for (const Aggressor& agg : Aggressors()) {
      DYNAMIC_SECTION(maker.name << " x " << agg.name) {
        OrderBook book;
        book.submit(maker.build(1, kMakerPrice, kMakerQty));
        const SubmitOutcome out = book.submit(agg.build(2, 6));

        REQUIRE(out.accepted);
        REQUIRE(out.filled == 6);
        REQUIRE(TradedQty(out) == 6);
        for (const Trade& t : out.trades) {
          REQUIRE(t.price == kMakerPrice);  // always the maker's price
        }
        REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == 4);
      }
    }
  }
}

TEST_CASE("Matrix: aggressor larger than maker; all-or-none rejects",
          "[matrix]") {
  for (const Maker& maker : Makers()) {
    for (const Aggressor& agg : Aggressors()) {
      DYNAMIC_SECTION(maker.name << " x " << agg.name) {
        OrderBook book;
        book.submit(maker.build(1, kMakerPrice, kMakerQty));
        const SubmitOutcome out = book.submit(agg.build(2, 15));

        if (agg.all_or_none) {
          // All-or-none never partially fills, so the maker is untouched.
          REQUIRE(out.filled == 0);
          REQUIRE(out.trades.empty());
          REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == kMakerQty);
          if (agg.immediate) {
            REQUIRE_FALSE(out.accepted);  // FOK rejects
          } else {
            REQUIRE(out.accepted);        // GTC AON rests as a maker
            REQUIRE(out.rested);
          }
        } else {
          REQUIRE(out.accepted);
          REQUIRE(out.filled == kMakerQty);  // consumes the whole maker
          REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == 0);
        }
      }
    }
  }
}

TEST_CASE("Post-only aggressor is rejected against every maker",
          "[matrix][flags]") {
  for (const Maker& maker : Makers()) {
    DYNAMIC_SECTION(maker.name) {
      OrderBook book;
      book.submit(maker.build(1, kMakerPrice, kMakerQty));
      Order po = MakeLimit(2, Side::Buy, kCrossPrice, 5, Tif::GTX);  // post-only
      const SubmitOutcome out = book.submit(po);
      REQUIRE_FALSE(out.accepted);
      REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == kMakerQty);
    }
  }
}

TEST_CASE("Min-quantity aggressor rejects when the floor is unavailable",
          "[matrix][flags]") {
  for (const Maker& maker : Makers()) {
    DYNAMIC_SECTION(maker.name) {
      OrderBook book;
      book.submit(maker.build(1, kMakerPrice, kMakerQty));
      Order o = MakeLimit(2, Side::Buy, kCrossPrice, 20);
      o.min_qty = 12;  // more than the 10 available
      const SubmitOutcome out = book.submit(o);
      REQUIRE_FALSE(out.accepted);
      REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == kMakerQty);
    }
  }
}

TEST_CASE("Pegged maker matches like a resting limit", "[matrix][peg]") {
  // A midpoint peg needs a reference BBO: bid 90 / ask 110 -> midpoint 100.
  auto make_book = []() {
    auto book = std::make_unique<OrderBook>();
    book->submit(MakeLimit(1, Side::Buy, 90, 10));
    book->submit(MakeLimit(2, Side::Sell, 110, 10));
    Order peg;
    peg.id = 3;
    peg.side = Side::Sell;
    peg.type = OrdType::Limit;
    peg.tif = Tif::GTC;
    peg.qty = kMakerQty;
    PegSpec p;
    p.ref = PegSpec::Ref::Midpoint;
    peg.peg = p;
    book->submit(peg);
    return book;
  };

  SECTION("partial fill by a limit that only reaches the peg") {
    auto book = make_book();
    REQUIRE(book->total_qty_at(Side::Sell, 100) == kMakerQty);  // peg at 100
    const SubmitOutcome out = book->submit(MakeLimit(4, Side::Buy, 100, 6));
    REQUIRE(out.filled == 6);
    REQUIRE(out.trades.size() == 1);
    REQUIRE(out.trades[0].price == 100);
    REQUIRE(book->total_qty_at(Side::Sell, 100) == 4);
  }
  SECTION("FOK against the peg is all-or-none") {
    auto book = make_book();
    const SubmitOutcome ok =
        book->submit(MakeLimit(4, Side::Buy, 100, 10, Tif::FOK));
    REQUIRE(ok.accepted);
    REQUIRE(ok.filled == 10);

    auto book2 = make_book();
    const SubmitOutcome rej =
        book2->submit(MakeLimit(4, Side::Buy, 100, 11, Tif::FOK));
    REQUIRE_FALSE(rej.accepted);  // only 10 available at the peg
  }
}

TEST_CASE("Resting AON maker is not partially filled", "[matrix][aon]") {
  OrderBook book;
  Order aon = MakeLimit(1, Side::Sell, kMakerPrice, kMakerQty);
  SetFlag(&aon.flags, OrderFlag::kAon);
  book.submit(aon);

  SECTION("an aggressor too small to fill it is not matched") {
    const SubmitOutcome out =
        book.submit(MakeLimit(2, Side::Buy, kMakerPrice, 4));
    REQUIRE(out.trades.empty());  // 4 cannot fully fill the AON maker of 10
    REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == kMakerQty);
  }
  SECTION("an aggressor large enough fills it in full") {
    const SubmitOutcome out =
        book.submit(MakeLimit(2, Side::Buy, kMakerPrice, 10));
    REQUIRE(out.filled == 10);
    REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == 0);
  }
}

TEST_CASE("Aggressor fills past an unfillable AON maker to orders behind it",
          "[matrix][aon]") {
  OrderBook book;
  // Best ask is an AON of 10; behind it at the same price sits a plain 5.
  Order aon = MakeLimit(1, Side::Sell, kMakerPrice, kMakerQty);
  SetFlag(&aon.flags, OrderFlag::kAon);
  book.submit(aon);
  book.submit(MakeLimit(2, Side::Sell, kMakerPrice, 5));

  // A buy of 5 cannot take the AON (needs 10) but fills the plain order behind.
  const SubmitOutcome out = book.submit(MakeLimit(3, Side::Buy, kMakerPrice, 5));
  REQUIRE(out.filled == 5);
  REQUIRE(out.trades.size() == 1);
  REQUIRE(out.trades[0].maker_id == 2);          // the order behind the AON
  REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == kMakerQty);  // AON left
}

TEST_CASE("Aggressor skips an AON best level to fill a worse level",
          "[matrix][aon]") {
  OrderBook book;
  // Best ask 100 is an AON of 10; a plain 5 rests one tick worse at 101.
  Order aon = MakeLimit(1, Side::Sell, 100, 10);
  SetFlag(&aon.flags, OrderFlag::kAon);
  book.submit(aon);
  book.submit(MakeLimit(2, Side::Sell, 101, 5));

  // A buy limit up to 101 for 5 cannot take the AON at 100, so it trades at 101.
  const SubmitOutcome out = book.submit(MakeLimit(3, Side::Buy, 101, 5));
  REQUIRE(out.filled == 5);
  REQUIRE(out.trades.size() == 1);
  REQUIRE(out.trades[0].price == 101);
  REQUIRE(book.total_qty_at(Side::Sell, 100) == 10);  // AON untouched
  REQUIRE(book.total_qty_at(Side::Sell, 101) == 0);
}

// ---- Stop-triggered aggressors vs each maker type -------------------------

TEST_CASE("A triggered stop-market matches every resting maker type",
          "[matrix][stop]") {
  for (const Maker& maker : Makers()) {
    DYNAMIC_SECTION(maker.name) {
      OrderBook book;
      // The stop's eventual target: a maker of 10 resting at 101.
      book.submit(maker.build(1, 101, 10));
      // Trigger liquidity at 100 so a trade there sets the last price.
      book.submit(MakeLimit(2, Side::Sell, 100, 1));
      // A parked buy stop-market; fires once the last price reaches 100.
      book.submit(MakeStopMarket(3, Side::Buy, 100, 5));
      REQUIRE(book.pending_stop_count() == 1);

      // This trade at 100 sets the last price and triggers the stop, whose
      // injected market buy then walks up to the maker at 101.
      const SubmitOutcome out = book.submit(MakeLimit(4, Side::Buy, 100, 1));
      REQUIRE(book.pending_stop_count() == 0);
      REQUIRE(HasTakerMaker(out, 3, 1));  // stop (3) executed against maker (1)
      REQUIRE(book.total_qty_at(Side::Sell, 101) == 5);  // 10 - 5 taken
    }
  }
}

TEST_CASE("A triggered stop-market sweeps an iceberg via slices",
          "[matrix][stop]") {
  OrderBook book;
  book.submit(MakeIceberg(1, Side::Sell, 101, 10, /*display=*/3));
  book.submit(MakeLimit(2, Side::Sell, 100, 1));
  book.submit(MakeStopMarket(3, Side::Buy, 100, 5));

  const SubmitOutcome out = book.submit(MakeLimit(4, Side::Buy, 100, 1));
  REQUIRE(book.pending_stop_count() == 0);
  // The stop's 5 comes from the iceberg in 3 + 2 slices, all taker = stop.
  Quantity from_stop = 0;
  for (const Trade& t : out.trades) {
    if (t.taker_id == 3) {
      from_stop += t.qty;
      REQUIRE(t.maker_id == 1);
      REQUIRE(t.price == 101);
    }
  }
  REQUIRE(from_stop == 5);
  REQUIRE(book.total_qty_at(Side::Sell, 101) == 5);
}

// ---- STP x (iceberg / hidden / pegged) ------------------------------------

TEST_CASE("STP cancel-resting removes the whole maker, whatever its type",
          "[matrix][stp]") {
  SECTION("iceberg maker") {
    OrderBook book(StpPolicy::kCancelResting);
    Order ice = MakeIceberg(1, Side::Sell, 100, 10, 3);
    ice.client_id = 5;
    book.submit(ice);
    Order buy = MakeLimit(2, Side::Buy, 100, 4);
    buy.client_id = 5;
    const SubmitOutcome out = book.submit(buy);

    REQUIRE(out.trades.empty());                    // no self-trade
    REQUIRE(book.find(1) == nullptr);               // whole iceberg cancelled
    REQUIRE(book.total_qty_at(Side::Sell, 100) == 0);
    REQUIRE(book.displayed_qty_at(Side::Sell, 100) == 0);  // displayed cleaned
    Ticks bid = 0;
    REQUIRE(book.best_bid(&bid));                   // aggressor rested instead
    REQUIRE(bid == 100);
  }
  SECTION("hidden maker") {
    OrderBook book(StpPolicy::kCancelResting);
    Order hid = MakeHidden(1, Side::Sell, 100, 10);
    hid.client_id = 5;
    book.submit(hid);
    Order buy = MakeLimit(2, Side::Buy, 100, 4);
    buy.client_id = 5;
    const SubmitOutcome out = book.submit(buy);
    REQUIRE(out.trades.empty());
    REQUIRE(book.find(1) == nullptr);
    REQUIRE(book.total_qty_at(Side::Sell, 100) == 0);
  }
  SECTION("pegged maker") {
    OrderBook book(StpPolicy::kCancelResting);
    book.submit(MakeLimit(1, Side::Buy, 90, 10));   // reference bid
    book.submit(MakeLimit(2, Side::Sell, 110, 10));  // reference ask
    Order peg = MakeMidpointPeg(3, Side::Sell, 10);  // rests at 100
    peg.client_id = 5;
    book.submit(peg);
    REQUIRE(book.total_qty_at(Side::Sell, 100) == 10);

    Order buy = MakeLimit(4, Side::Buy, 100, 5);
    buy.client_id = 5;
    const SubmitOutcome out = book.submit(buy);
    REQUIRE(out.trades.empty());
    REQUIRE(book.find(3) == nullptr);               // peg cancelled
    REQUIRE(book.total_qty_at(Side::Sell, 100) == 0);
  }
}

TEST_CASE("STP cancel-aggressor leaves an iceberg maker intact",
          "[matrix][stp]") {
  OrderBook book(StpPolicy::kCancelAggressor);
  Order ice = MakeIceberg(1, Side::Sell, 100, 10, 3);
  ice.client_id = 5;
  book.submit(ice);
  Order buy = MakeLimit(2, Side::Buy, 100, 4);
  buy.client_id = 5;
  const SubmitOutcome out = book.submit(buy);

  REQUIRE(out.trades.empty());
  REQUIRE_FALSE(out.rested);                        // aggressor cancelled
  REQUIRE(book.find(1) != nullptr);                 // iceberg untouched
  REQUIRE(book.total_qty_at(Side::Sell, 100) == 10);
}

// ---- iceberg x pegged at the same price -----------------------------------

TEST_CASE("An iceberg and a pegged order coexist and match in FIFO order",
          "[matrix][peg][iceberg]") {
  OrderBook book;
  // The iceberg is the best (non-pegged) ask at 100; a primary-pegged sell
  // joins it there (a midpoint peg would instead sit inside the spread).
  book.submit(MakeIceberg(1, Side::Sell, 100, 6, 3));  // first in queue
  book.submit(MakePrimaryPeg(2, Side::Sell, 4));       // pegs to the 100 ask

  // Level 100 holds iceberg(6) + peg(4); displayed = slice(3) + peg(4) = 7.
  REQUIRE(book.total_qty_at(Side::Sell, 100) == 10);
  REQUIRE(book.displayed_qty_at(Side::Sell, 100) == 7);

  // A buy of 8 takes the iceberg's slice, then the peg (behind the re-queued
  // iceberg), then the iceberg's next slice: 3 + 4 + 1.
  const SubmitOutcome out = book.submit(MakeLimit(3, Side::Buy, 100, 8));
  REQUIRE(out.filled == 8);
  REQUIRE(book.find(2) == nullptr);                  // peg fully filled
  REQUIRE(book.total_qty_at(Side::Sell, 100) == 2);  // iceberg reserve left
}
