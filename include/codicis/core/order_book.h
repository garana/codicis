#ifndef CODICIS_CORE_ORDER_BOOK_H
#define CODICIS_CORE_ORDER_BOOK_H

/**
 * @file order_book.h
 * @brief Single-instrument continuous limit order book and matcher.
 *
 * Price-time priority matching for Market and Limit orders, with the core
 * time-in-force behaviors (GTC rests, IOC discards the remainder, FOK/AON is
 * all-or-none) and Post-Only rejection. Trade price is always the resting
 * (maker) price. The book is a dense price "ladder" giving O(1) price->level
 * lookup, with FIFO time priority within each level and O(1) cancel.
 *
 * The engine is synchronous and deterministic: it decides only on resident
 * state. Storage reporting and the async top-of-book window are layered above
 * this class (see StorageClient) and are not part of matching.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "codicis/core/order.h"
#include "codicis/core/types.h"

namespace codicis {

/**
 * @brief A single execution between an aggressor and a resting order.
 */
struct Trade {
  OrderId taker_id;   /**< The aggressing order. */
  OrderId maker_id;   /**< The resting order that was hit. */
  Side taker_side;    /**< Side of the aggressor. */
  Ticks price;        /**< Execution price (the maker's price). */
  Quantity qty;       /**< Executed quantity. */
};

/**
 * @brief The result of submitting an order.
 */
struct SubmitOutcome {
  bool accepted = true;         /**< False if the order was rejected. */
  std::string reject_reason;    /**< Populated when !accepted. */
  std::vector<Trade> trades;    /**< Executions produced, incl. cascades. */
  Quantity filled = 0;          /**< Total quantity executed. */
  bool rested = false;          /**< True if a remainder rests in the book. */
  bool pending_trigger = false; /**< True if parked awaiting a stop trigger. */
  bool held = false;            /**< True if held as an OTO/bracket child. */
  OrderId order_id = 0;         /**< The submitted order's id. */
};

/**
 * @brief Self-trade prevention policy applied when an order would match its
 *        own account (same non-zero client_id).
 */
enum class StpPolicy {
  kNone,            /**< Disabled: same-account orders may trade. */
  kCancelResting,   /**< Cancel the resting maker; do not trade; continue. */
  kCancelAggressor, /**< Cancel the incoming remainder; stop matching. */
  kCancelBoth,      /**< Cancel both the maker and the aggressor remainder. */
};

/**
 * @brief A continuous limit order book for one instrument.
 */
class OrderBook {
 public:
  OrderBook();

  /**
   * @brief Construct with a self-trade prevention policy.
   * @param stp The policy to apply on same-account crossings.
   */
  explicit OrderBook(StpPolicy stp);

  ~OrderBook();

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  /**
   * @brief Submit an order for matching (and resting, if applicable).
   *
   * The order is normalized and assigned an arrival sequence number. It is
   * matched against the opposite side per price-time priority; any remainder
   * rests unless the order is Market or IOC. FOK/AON orders that cannot be
   * fully filled immediately are rejected with no trades. Post-Only orders
   * that would cross are rejected.
   * @param order The order to submit (id must be set and unique).
   * @return The outcome: trades, fill quantity, and rest/reject status.
   */
  SubmitOutcome submit(Order order);

  /**
   * @brief Cancel a resting order.
   * @param id The order id.
   * @return True if the order was resting and is now removed.
   */
  bool cancel(OrderId id);

  /**
   * @brief Find a resting order by id.
   * @param id The order id.
   * @return Pointer to the resting order, or nullptr if not resting.
   */
  const Order* find(OrderId id) const;

  /**
   * @brief Get the best bid price.
   * @param out Receives the best bid on success.
   * @return True if there is at least one resting bid.
   */
  bool best_bid(Ticks* out) const;

  /**
   * @brief Get the best ask price.
   * @param out Receives the best ask on success.
   * @return True if there is at least one resting ask.
   */
  bool best_ask(Ticks* out) const;

  /**
   * @brief Total resting (matchable) quantity at a price on a side.
   *
   * Includes hidden orders and the full reserve of icebergs -- this is what
   * the matcher can execute against.
   * @param side  The book side.
   * @param price The price level.
   * @return The summed leaves quantity at that level.
   */
  Quantity total_qty_at(Side side, Ticks price) const;

  /**
   * @brief Displayed quantity at a price on a side (the public book view).
   *
   * Excludes hidden orders entirely and counts only an iceberg's currently
   * visible slice.
   * @param side  The book side.
   * @param price The price level.
   * @return The summed displayed quantity at that level.
   */
  Quantity displayed_qty_at(Side side, Ticks price) const;

  /**
   * @brief Cancel all resting orders whose expiry is at or before @p now.
   * @param now The current timestamp (nanoseconds).
   * @return The ids of the orders that were expired, in ascending id order.
   */
  std::vector<OrderId> expire(Timestamp now);

  /**
   * @brief Get the last trade price (the stop-trigger reference).
   * @param out Receives the last trade price on success.
   * @return True if at least one trade has occurred.
   */
  bool last_trade_price(Ticks* out) const;

  /** @return The number of resting orders across both sides. */
  std::size_t resting_count() const;

  /** @return The number of stop/trailing orders awaiting a trigger. */
  std::size_t pending_stop_count() const;

 private:
  /**
   * @brief React to fills of contingent (OCO/OTO/bracket) orders.
   *
   * Cancels OCO siblings and releases OTO/bracket children (which may match,
   * appending their trades to @p out).
   * @param submitted The order just submitted.
   * @param out       The outcome to augment with cascade trades.
   */
  void process_link_fills(const Order& submitted, SubmitOutcome& out);

  /**
   * @brief Release the pending children of a group whose parent has filled.
   * @param group_id The contingent group id.
   * @param out      The outcome to augment with any child trades.
   */
  void release_children(std::uint64_t group_id, SubmitOutcome& out);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace codicis

#endif  // CODICIS_CORE_ORDER_BOOK_H
