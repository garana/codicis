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
