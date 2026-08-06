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

#include "codicis/core/book_event.h"
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
  bool rested = false;          /**< True if a remainder rests resident. */
  bool rested_deep = false;     /**< True if it rests beyond the resident
                                     window (persisted, not in memory). */
  bool pending_trigger = false; /**< True if parked awaiting a stop trigger. */
  bool held = false;            /**< True if held as an OTO/bracket child. */
  OrderId order_id = 0;         /**< The submitted order's id. */
  std::vector<Order> evicted;   /**< Resting orders pushed out of the resident
                                     window by this order (now deep). */
  Order resting;                /**< The resting order (final price/leaves/seq),
                                     valid when rested or rested_deep. */
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

  /**
   * @brief Construct with an STP policy and a resident-window bound.
   * @param stp        The self-trade prevention policy.
   * @param mem_levels Max populated price levels kept resident per side
   *                   (0 = unbounded; orders beyond the window rest "deep",
   *                   persisted but not held in memory).
   */
  OrderBook(StpPolicy stp, std::size_t mem_levels);

  ~OrderBook();

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  /**
   * @brief Set the instrument symbol used to tag emitted book events.
   * @param symbol The instrument this book trades.
   */
  void set_symbol(Symbol symbol);

  /**
   * @brief Seed the arrival/priority sequence counter (boot restart safety).
   *
   * Advances the internal `next_seq` to at least @p next so that every new
   * order's `seq`/`rank` exceeds anything restored from storage -- preventing a
   * post-restart order from jumping ahead of older resting orders. Monotonic:
   * never lowers the counter.
   * @param next The next sequence value to hand out (typically max rank + 1).
   */
  void set_next_seq(SeqNo next);

  /**
   * @brief Take the orders that moved to the back of a level since the last
   *        call, each with a freshly re-stamped priority @c rank.
   *
   * Populated by peg reprices and iceberg replenishes (which lose time
   * priority). The engine drains this after every operation to update the
   * order's storage priority (report_rest with the new rank/price), so
   * pull_levels reconstructs the correct queue order. Clears the buffer.
   * @return The requeued orders (empty if none moved).
   */
  std::vector<Order> take_requeued();

  /**
   * @brief Install the book-event sink (nullptr disables emission).
   *
   * When set, the book calls @p sink synchronously for every Add/Cancel/Trade
   * mutation, in application order. The @c seq is left 0 for the caller (the
   * MatchingEngine) to stamp with a global monotonic value.
   * @param sink The sink, or nullptr to stop emitting.
   */
  void set_event_sink(BookEventSink* sink);

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

  /**
   * @brief Seed an account's net position (for reduce-only clamping).
   *
   * Sets the client's net position outright, used to load the authoritative
   * position from storage before the client's first order is placed (when it
   * has no book presence, so no fills can race the seed). A positive value is
   * long, negative is short. Anonymous (client 0) is ignored.
   * @param client The account (client id).
   * @param net    The net position to set.
   */
  void seed_position(ClientId client, Quantity net);

  /**
   * @brief Get an account's current net position (+long / -short).
   * @param client The account (client id).
   * @return The net position, or 0 if the account is unknown.
   */
  Quantity position(ClientId client) const;

  /**
   * @brief Insert a resting order pulled back from storage, WITHOUT matching.
   *
   * Used to re-materialize a deep level into the resident window. The order is
   * assumed already resting (its leaves/seq are authoritative); it is placed
   * into the ladder and index directly, not run through the matcher.
   * @param order The resting order to materialize.
   */
  void insert_resident(const Order& order);

  /**
   * @brief Whether deep (non-resident) liquidity exists on a side.
   * @param side The book side.
   * @return True if orders rest beyond the resident window on that side.
   */
  bool has_deep(Side side) const;

  /**
   * @brief The nearest deep price on a side (just beyond the resident window).
   * @param side The book side.
   * @param out  Receives the price on success.
   * @return True if there is deep liquidity on that side.
   */
  bool deep_boundary(Side side, Ticks* out) const;

  /**
   * @brief The worst (farthest-from-top) resident price on a side.
   * @param side The book side.
   * @param out  Receives the price on success.
   * @return True if the side has any resident order.
   */
  bool worst_resident(Side side, Ticks* out) const;

  /**
   * @brief Run the opening uniform-price auction over queued MOO/LOO/OPG orders.
   *
   * Computes a single clearing price that maximizes executed volume (ties
   * broken by smallest order imbalance, then nearest the last trade price, then
   * lowest price) and crosses all eligible orders at that one price. Unfilled
   * LOO limit remainders enter continuous trading; MOO market and OPG remainders
   * are discarded. Resulting trades may cascade stops and reprice pegs.
   * @return The auction executions (empty if nothing crosses).
   */
  std::vector<Trade> run_opening_auction();

  /**
   * @brief Run the closing uniform-price auction over queued MOC/LOC orders.
   *
   * Same uniform-price cross as the opening auction; unfilled LOC remainders
   * enter continuous trading, MOC remainders are discarded.
   * @return The auction executions (empty if nothing crosses).
   */
  std::vector<Trade> run_closing_auction();

  /**
   * @brief Number of orders queued for an auction.
   * @param opening True for the opening auction, false for the closing one.
   * @return The queued order count.
   */
  std::size_t auction_count(bool opening) const;

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
