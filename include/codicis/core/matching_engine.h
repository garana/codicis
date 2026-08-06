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

#include "codicis/core/book_event.h"
#include "codicis/core/order.h"
#include "codicis/core/order_book.h"
#include "codicis/core/types.h"

namespace codicis {

/**
 * @brief Routes order-book operations to a per-symbol @ref OrderBook.
 *
 * It is itself the @ref BookEventSink for every book it owns: each book stamps
 * the symbol and calls back here, where a single global monotonic sequence is
 * assigned before forwarding to the registered downstream sink (the feed). One
 * feed clock across all instruments; a per-symbol consumer filters by symbol.
 */
class MatchingEngine : public BookEventSink {
 public:
  /**
   * @brief Construct with a self-trade prevention policy for all books.
   * @param stp        The policy applied to every created book.
   * @param mem_levels Resident-window bound per side for every book
   *                   (0 = unbounded).
   */
  explicit MatchingEngine(StpPolicy stp = StpPolicy::kNone,
                          std::size_t mem_levels = 0)
      : stp_(stp), mem_levels_(mem_levels) {}

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

  /**
   * @brief Run a symbol's opening uniform-price auction.
   * @param symbol The instrument (book created on first use).
   * @return The auction executions.
   */
  std::vector<Trade> run_opening_auction(const Symbol& symbol);

  /**
   * @brief Run a symbol's closing uniform-price auction.
   * @param symbol The instrument (book created on first use).
   * @return The auction executions.
   */
  std::vector<Trade> run_closing_auction(const Symbol& symbol);

  /**
   * @brief Number of orders queued for a symbol's auction.
   * @param symbol  The instrument.
   * @param opening True for the opening auction, false for the closing one.
   * @return The queued order count (0 for an unknown symbol).
   */
  std::size_t auction_count(const Symbol& symbol, bool opening) const;

  /**
   * @brief Materialize a pulled-back resting order into a symbol's book.
   * @param symbol The instrument (book created on first use).
   * @param order  The resting order (from storage) to insert without matching.
   */
  void insert_resident(const Symbol& symbol, const Order& order);

  /**
   * @brief Whether deep (non-resident) liquidity exists on a symbol's side.
   * @param symbol The instrument.
   * @param side   The book side.
   * @return True if there are deep orders on that side.
   */
  bool has_deep(const Symbol& symbol, Side side) const;

  /**
   * @brief The worst (farthest-from-top) resident price on a symbol's side.
   * @param symbol The instrument.
   * @param side   The book side.
   * @param out    Receives the price on success.
   * @return True if that side has any resident order.
   */
  bool worst_resident(const Symbol& symbol, Side side, Ticks* out) const;

  /**
   * @brief Take the orders re-queued (peg reprice / iceberg replenish) in a
   *        symbol's book since the last call, with re-stamped ranks.
   * @param symbol The instrument.
   * @return The requeued orders (empty for an unknown symbol or none moved).
   */
  std::vector<Order> take_requeued(const Symbol& symbol);

  /** @return Whether a book exists for the symbol. */
  bool has_symbol(const Symbol& symbol) const {
    return books_.find(symbol) != books_.end();
  }

  /** @return The number of instruments with a book. */
  std::size_t symbol_count() const { return books_.size(); }

  /**
   * @brief Set the sequence base applied to every book created afterward, and
   *        to existing books, so new orders' seq/rank exceed restored ones.
   * @param base The next sequence value (typically the storage max rank + 1).
   */
  void set_seq_base(SeqNo base);

  /**
   * @brief Register the downstream book-event consumer (nullptr to detach).
   *
   * The engine installs itself as each book's sink; events flow book -> engine
   * (seq stamped) -> @p sink. Books created after this call inherit emission.
   * @param sink The feed consumer, or nullptr to stop forwarding.
   */
  void set_book_event_sink(BookEventSink* sink) { feed_sink_ = sink; }

  /** @return The last global book-event sequence number assigned (0 if none). */
  SeqNo last_event_seq() const { return event_seq_; }

  /** @brief BookEventSink: stamp a global seq and forward to the feed sink. */
  void on_book_event(const BookEvent& ev) override;

 private:
  /** @return The book for @p symbol, creating it if absent. */
  OrderBook& book_for(const Symbol& symbol);

  /** @return The book for @p symbol, or nullptr if none exists. */
  const OrderBook* book_of(const Symbol& symbol) const;

  StpPolicy stp_;
  std::size_t mem_levels_ = 0;
  std::unordered_map<Symbol, std::unique_ptr<OrderBook>> books_;
  BookEventSink* feed_sink_ = nullptr;  // downstream feed consumer (optional)
  SeqNo event_seq_ = 0;                 // global monotonic feed sequence
  SeqNo seq_base_ = 1;                  // seed for new books (boot watermark)
};

}  // namespace codicis

#endif  // CODICIS_CORE_MATCHING_ENGINE_H
