#ifndef CODICIS_FEED_BOOK_REPLICA_H
#define CODICIS_FEED_BOOK_REPLICA_H

/**
 * @file book_replica.h
 * @brief An order-book replica built by applying the book-event stream.
 *
 * The feed-helper feeds every decoded @ref BookEvent into a BookReplica, which
 * mechanically maintains, per symbol, the market-by-order (L3) map and the
 * aggregated depth (L2) from which best-bid/ask (L1) is read. It runs NO
 * matching -- it only applies the deltas the matcher already decided.
 *
 * Depth here is the MATCHABLE book (leaves), i.e. hidden orders and the full
 * iceberg reserve are included; a displayed-only view would need a displayed
 * quantity carried on Add/Reprice and is a follow-on. Because the upstream feed
 * is best-effort, @ref apply detects a @c seq gap so the helper can flag its
 * replica stale and (later) resync from a snapshot.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "codicis/core/book_event.h"
#include "codicis/core/types.h"

namespace codicis {

/**
 * @brief A per-symbol L3/L2/L1 replica driven by the book-event stream.
 */
class BookReplica {
 public:
  /** @brief One aggregated price level (L2). */
  struct Level {
    Ticks price = 0;
    Quantity qty = 0;
  };

  /** @brief One resting order (L3 / market-by-order). */
  struct OrderView {
    OrderId id = 0;
    Side side = Side::Buy;
    Ticks price = 0;
    Quantity qty = 0;
  };

  /**
   * @brief Apply one event, updating the replica.
   * @param ev The decoded book event.
   * @return True if a sequence gap was detected (an event was lost upstream);
   *         the replica is then potentially stale until a resync.
   */
  bool apply(const BookEvent& ev);

  /**
   * @brief Best bid (L1) for a symbol.
   * @param symbol The instrument.
   * @param price  Receives the best bid price on success.
   * @param qty    Receives the aggregated size at that price on success.
   * @return True if the symbol has a resting bid.
   */
  bool best_bid(const Symbol& symbol, Ticks* price, Quantity* qty) const;

  /**
   * @brief Best ask (L1) for a symbol.
   * @param symbol The instrument.
   * @param price  Receives the best ask price on success.
   * @param qty    Receives the aggregated size at that price on success.
   * @return True if the symbol has a resting ask.
   */
  bool best_ask(const Symbol& symbol, Ticks* price, Quantity* qty) const;

  /**
   * @brief Aggregated depth (L2) for a side, best price first.
   * @param symbol The instrument.
   * @param side   The book side.
   * @param depth  Maximum levels to return (0 = all).
   * @return The levels, best first (highest bid / lowest ask).
   */
  std::vector<Level> depth(const Symbol& symbol, Side side,
                           std::size_t depth) const;

  /**
   * @brief Resting orders (L3) at a price, in no particular order.
   * @param symbol The instrument.
   * @param side   The book side.
   * @param price  The price level.
   * @return The resting orders at that level (empty if none).
   */
  std::vector<OrderView> orders_at(const Symbol& symbol, Side side,
                                   Ticks price) const;

  /** @return The highest sequence number applied (0 if none). */
  SeqNo last_seq() const { return last_seq_; }

  /** @return The number of sequence gaps detected since construction. */
  std::uint64_t gaps() const { return gaps_; }

  /** @return The number of symbols with any state. */
  std::size_t symbol_count() const { return books_.size(); }

 private:
  /** @brief Per-symbol replica state. */
  struct SymBook {
    std::map<Ticks, Quantity> bids;  // price -> aggregated qty (asc; best=last)
    std::map<Ticks, Quantity> asks;  // price -> aggregated qty (asc; best=first)
    std::unordered_map<OrderId, OrderView> orders;  // L3 by id
  };

  /** @return The side's price->qty map within a symbol book. */
  static std::map<Ticks, Quantity>& side_map(SymBook& b, Side s) {
    return s == Side::Buy ? b.bids : b.asks;
  }

  /** @brief Add @p delta to a level, erasing it when it reaches zero. */
  static void bump(std::map<Ticks, Quantity>& m, Ticks price, Quantity delta);

  std::unordered_map<Symbol, SymBook> books_;
  SeqNo last_seq_ = 0;
  std::uint64_t gaps_ = 0;
};

}  // namespace codicis

#endif  // CODICIS_FEED_BOOK_REPLICA_H
