#ifndef CODICIS_CORE_MATCHING_ENGINE_H
#define CODICIS_CORE_MATCHING_ENGINE_H

/**
 * @file matching_engine.h
 * @brief A per-symbol registry of order books.
 *
 * MatchingEngine owns one @ref OrderBook per instrument, routing each
 * operation to the book for a given @ref Symbol. Books are created on first
 * use. The symbol is supplied explicitly on every call and is deliberately
 * NOT stored on the @ref Order struct: each book already knows its own
 * instrument, cancels are scoped by symbol, and keeping the symbol off the hot
 * order keeps it lean. Order ids are the caller's responsibility and only need
 * to be unique within a symbol's book.
 */

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "codicis/core/order.h"
#include "codicis/core/order_book.h"
#include "codicis/core/types.h"

namespace codicis {

/**
 * @brief Routes order-book operations to a per-symbol @ref OrderBook.
 */
class MatchingEngine {
 public:
  /**
   * @brief Construct with a self-trade prevention policy for all books.
   * @param stp The policy applied to every created book.
   */
  explicit MatchingEngine(StpPolicy stp = StpPolicy::kNone) : stp_(stp) {}

  /**
   * @brief Submit an order to a symbol's book (created on first use).
   * @param symbol The instrument.
   * @param order  The order to submit.
   * @return The matching outcome.
   */
  SubmitOutcome submit(const Symbol& symbol, Order order);

  /**
   * @brief Cancel a resting order in a symbol's book.
   * @param symbol The instrument.
   * @param id     The order id.
   * @return True if the order was resting and is now removed.
   */
  bool cancel(const Symbol& symbol, OrderId id);

  /**
   * @brief Find a resting order in a symbol's book.
   * @param symbol The instrument.
   * @param id     The order id.
   * @return Pointer to the resting order, or nullptr (unknown symbol or id).
   */
  const Order* find(const Symbol& symbol, OrderId id) const;

  /**
   * @brief Best bid for a symbol.
   * @param symbol The instrument.
   * @param out    Receives the best bid on success.
   * @return True if the symbol has a book with a resting bid.
   */
  bool best_bid(const Symbol& symbol, Ticks* out) const;

  /**
   * @brief Best ask for a symbol.
   * @param symbol The instrument.
   * @param out    Receives the best ask on success.
   * @return True if the symbol has a book with a resting ask.
   */
  bool best_ask(const Symbol& symbol, Ticks* out) const;

  /**
   * @brief Total resting quantity at a price for a symbol.
   * @param symbol The instrument.
   * @param side   The book side.
   * @param price  The price level.
   * @return The summed leaves quantity (0 for an unknown symbol).
   */
  Quantity total_qty_at(const Symbol& symbol, Side side, Ticks price) const;

  /**
   * @brief Displayed quantity at a price for a symbol.
   * @param symbol The instrument.
   * @param side   The book side.
   * @param price  The price level.
   * @return The summed displayed quantity (0 for an unknown symbol).
   */
  Quantity displayed_qty_at(const Symbol& symbol, Side side,
                            Ticks price) const;

  /**
   * @brief Expire timed-out resting orders in a symbol's book.
   * @param symbol The instrument.
   * @param now    The current timestamp (nanoseconds).
   * @return The ids of the orders that were expired.
   */
  std::vector<OrderId> expire(const Symbol& symbol, Timestamp now);

  /**
   * @brief Number of resting orders for a symbol.
   * @param symbol The instrument.
   * @return The resting order count (0 for an unknown symbol).
   */
  std::size_t resting_count(const Symbol& symbol) const;

  /**
   * @brief Seed an account's net position in a symbol's book.
   * @param symbol The instrument (book created on first use).
   * @param client The account (client id).
   * @param net    The net position to set (+long / -short).
   */
  void seed_position(const Symbol& symbol, ClientId client, Quantity net);

  /**
   * @brief An account's net position in a symbol's book.
   * @param symbol The instrument.
   * @param client The account (client id).
   * @return The net position (0 for an unknown symbol or account).
   */
  Quantity position(const Symbol& symbol, ClientId client) const;

  /** @return Whether a book exists for the symbol. */
  bool has_symbol(const Symbol& symbol) const {
    return books_.find(symbol) != books_.end();
  }

  /** @return The number of instruments with a book. */
  std::size_t symbol_count() const { return books_.size(); }

 private:
  /** @return The book for @p symbol, creating it if absent. */
  OrderBook& book_for(const Symbol& symbol);

  /** @return The book for @p symbol, or nullptr if none exists. */
  const OrderBook* book_of(const Symbol& symbol) const;

  StpPolicy stp_;
  std::unordered_map<Symbol, std::unique_ptr<OrderBook>> books_;
};

}  // namespace codicis

#endif  // CODICIS_CORE_MATCHING_ENGINE_H
