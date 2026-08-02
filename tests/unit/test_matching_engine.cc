/**
 * @file test_matching_engine.cc
 * @brief Unit tests for the per-symbol MatchingEngine registry.
 */

#include "catch_amalgamated.hpp"

#include "codicis/core/matching_engine.h"
#include "codicis/core/order.h"

using namespace codicis;

namespace {

Order Limit(OrderId id, Side side, Ticks price, Quantity qty) {
  Order o;
  o.id = id;
  o.side = side;
  o.type = OrdType::Limit;
  o.tif = Tif::GTC;
  o.price = price;
  o.qty = qty;
  return o;
}

}  // namespace

TEST_CASE("Books are created per symbol on first use", "[core][engine]") {
  MatchingEngine engine;
  REQUIRE(engine.symbol_count() == 0);
  REQUIRE_FALSE(engine.has_symbol("BTC"));

  engine.submit("BTC", Limit(1, Side::Buy, 100, 5));
  REQUIRE(engine.symbol_count() == 1);
  REQUIRE(engine.has_symbol("BTC"));
  REQUIRE_FALSE(engine.has_symbol("ETH"));
}

TEST_CASE("Orders in one symbol do not match another", "[core][engine]") {
  MatchingEngine engine;
  engine.submit("BTC", Limit(1, Side::Sell, 100, 10));
  // A buy that would cross the BTC ask, but on a different symbol.
  SubmitOutcome out = engine.submit("ETH", Limit(2, Side::Buy, 100, 10));

  REQUIRE(out.trades.empty());          // no cross-symbol matching
  REQUIRE(out.rested);
  REQUIRE(engine.total_qty_at("BTC", Side::Sell, 100) == 10);  // untouched
  REQUIRE(engine.total_qty_at("ETH", Side::Buy, 100) == 10);
}

TEST_CASE("Matching happens within a symbol", "[core][engine]") {
  MatchingEngine engine;
  engine.submit("BTC", Limit(1, Side::Sell, 100, 10));
  SubmitOutcome out = engine.submit("BTC", Limit(2, Side::Buy, 100, 4));
  REQUIRE(out.filled == 4);
  REQUIRE(out.trades.size() == 1);
  REQUIRE(out.trades[0].maker_id == 1);
  REQUIRE(engine.total_qty_at("BTC", Side::Sell, 100) == 6);
}

TEST_CASE("Top of book is per symbol", "[core][engine]") {
  MatchingEngine engine;
  engine.submit("BTC", Limit(1, Side::Buy, 99, 5));
  engine.submit("ETH", Limit(2, Side::Buy, 42, 5));

  Ticks bid = 0;
  REQUIRE(engine.best_bid("BTC", &bid));
  REQUIRE(bid == 99);
  REQUIRE(engine.best_bid("ETH", &bid));
  REQUIRE(bid == 42);
  REQUIRE_FALSE(engine.best_bid("DOGE", &bid));  // unknown symbol
}

TEST_CASE("Cancel is scoped to the symbol", "[core][engine]") {
  MatchingEngine engine;
  engine.submit("BTC", Limit(1, Side::Buy, 99, 5));
  engine.submit("ETH", Limit(1, Side::Buy, 99, 5));  // same id, other symbol

  REQUIRE(engine.cancel("BTC", 1));
  REQUIRE(engine.find("BTC", 1) == nullptr);
  REQUIRE(engine.find("ETH", 1) != nullptr);  // ETH's order untouched
  REQUIRE_FALSE(engine.cancel("DOGE", 1));    // unknown symbol
}

TEST_CASE("Unknown symbol queries are empty, not errors", "[core][engine]") {
  MatchingEngine engine;
  Ticks px = 0;
  REQUIRE_FALSE(engine.best_ask("NONE", &px));
  REQUIRE(engine.total_qty_at("NONE", Side::Buy, 100) == 0);
  REQUIRE(engine.displayed_qty_at("NONE", Side::Sell, 100) == 0);
  REQUIRE(engine.find("NONE", 1) == nullptr);
  REQUIRE(engine.expire("NONE", 1000).empty());
}
