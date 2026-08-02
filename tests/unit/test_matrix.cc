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

/** @brief A resting maker kind (rests total kMakerQty on the ask at 100). */
struct Maker {
  std::string name;
  std::function<Order(OrderId)> build;
};

/** @brief An aggressor kind (a crossing buy of a given quantity). */
struct Aggressor {
  std::string name;
  bool all_or_none;  // rejects rather than partially filling
  std::function<Order(OrderId, Quantity)> build;
};

std::vector<Maker> Makers() {
  return {
      {"limit",
       [](OrderId id) { return MakeLimit(id, Side::Sell, kMakerPrice, kMakerQty); }},
      {"iceberg",
       [](OrderId id) {
         Order o = MakeLimit(id, Side::Sell, kMakerPrice, kMakerQty);
         SetFlag(&o.flags, OrderFlag::kIceberg);
         o.display_qty = 3;
         return o;
       }},
      {"hidden",
       [](OrderId id) {
         Order o = MakeLimit(id, Side::Sell, kMakerPrice, kMakerQty);
         SetFlag(&o.flags, OrderFlag::kHidden);
         return o;
       }},
  };
}

std::vector<Aggressor> Aggressors() {
  return {
      {"limit", false,
       [](OrderId id, Quantity q) {
         return MakeLimit(id, Side::Buy, kCrossPrice, q);
       }},
      {"market", false,
       [](OrderId id, Quantity q) { return MakeMarket(id, Side::Buy, q); }},
      {"ioc", false,
       [](OrderId id, Quantity q) {
         return MakeLimit(id, Side::Buy, kCrossPrice, q, Tif::IOC);
       }},
      {"fok", true,
       [](OrderId id, Quantity q) {
         return MakeLimit(id, Side::Buy, kCrossPrice, q, Tif::FOK);
       }},
      {"aon", true,
       [](OrderId id, Quantity q) {
         Order o = MakeLimit(id, Side::Buy, kCrossPrice, q);
         SetFlag(&o.flags, OrderFlag::kAon);
         return o;
       }},
      {"minqty", false,
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
        book.submit(maker.build(1));
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
        book.submit(maker.build(1));
        const SubmitOutcome out = book.submit(agg.build(2, 15));

        if (agg.all_or_none) {
          REQUIRE_FALSE(out.accepted);  // cannot fully fill 15 against 10
          REQUIRE(out.trades.empty());
          REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == kMakerQty);
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
      book.submit(maker.build(1));
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
      book.submit(maker.build(1));
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

// KNOWN GAP: a resting all-or-none order should only ever be filled in full
// (the matcher should skip it when the aggressor cannot take its whole size).
// That maker-side AON handling is not implemented yet -- AON is enforced only
// on the aggressor. This test asserts the correct behavior and is expected to
// FAIL until maker-side AON is added; [!shouldfail] tracks it.
TEST_CASE("Resting AON maker is not partially filled", "[matrix][!shouldfail]") {
  OrderBook book;
  Order aon = MakeLimit(1, Side::Sell, kMakerPrice, kMakerQty);
  SetFlag(&aon.flags, OrderFlag::kAon);
  book.submit(aon);

  const SubmitOutcome out = book.submit(MakeLimit(2, Side::Buy, kMakerPrice, 4));
  REQUIRE(out.trades.empty());  // 4 cannot fully fill the AON maker of 10
  REQUIRE(book.total_qty_at(Side::Sell, kMakerPrice) == kMakerQty);
}
