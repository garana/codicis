#ifndef CODICIS_CORE_BOOK_EVENT_H
#define CODICIS_CORE_BOOK_EVENT_H

/**
 * @file book_event.h
 * @brief Sequence-numbered order-book mutation events (feed foundation).
 *
 * The matching engine emits one @ref BookEvent for every mutation a public
 * market-data feed must observe, in the exact order the book applied them. This
 * is the raw stream the (future) feed-helper turns into L1/L2/L3. It is a
 * best-effort, non-blocking side channel -- distinct from the reliable, acked
 * storage path; a consumer that falls behind detects a @c seq gap and resyncs
 * from a snapshot rather than stalling the matcher.
 *
 * Slice 1 was Add / Cancel / Trade -- the backbone of book depth. Slice 2 adds
 * the three remaining mutation kinds that change an already-resting order in
 * place: Reprice (a peg moves), Replenish (an iceberg refreshes its slice), and
 * Trigger (a parked stop fires). Prints are anonymous (price/qty/side, order
 * ids for internal/drop-copy use only).
 */

#include <cstdint>

#include "codicis/core/types.h"

namespace codicis {

/** @brief The kind of book mutation an event reports. */
enum class BookEventType : std::uint8_t {
  kAdd,       /**< An order joined the book (rests resident or deep). */
  kCancel,    /**< A resting order left the book other than by a fill
                   (explicit cancel, GTD/DAY expiry, STP- or OCO-cancel). */
  kTrade,     /**< An execution between an aggressor and a resting order. */
  kReprice,   /**< A resting pegged order moved to a new price (prev_price ->
                   price), losing time priority: decrement the old level,
                   add its leaves at the new one. */
  kReplenish, /**< An iceberg exhausted its displayed slice and refreshed it
                   from the hidden reserve, re-queued at the back of its level.
                   qty is the NEW displayed slice at price. */
  kTrigger,   /**< A parked stop/stop-limit/trailing order fired and was
                   injected as a market/limit order. A lifecycle marker: the
                   resulting depth/executions arrive as its own Add/Trade
                   events. price is the order's limit (0 for stop-market). */
};

/**
 * @brief One order-book mutation, tagged with a global monotonic sequence.
 *
 * @c seq is assigned by the MatchingEngine across all symbols (a single feed
 * clock); a per-symbol consumer simply filters by @c symbol and tolerates the
 * gaps left by other instruments. Field meaning depends on @c type:
 * - kAdd:    order_id/side/price rest; qty is the resting leaves.
 * - kCancel: order_id/side/price of the removed order; qty is its leaves.
 * - kTrade:  maker_id (== order_id) is the resting order hit, taker_id the
 *            aggressor, side the aggressor side, price the maker price, qty the
 *            executed quantity.
 */
struct BookEvent {
  SeqNo seq = 0;                            /**< Global feed sequence number. */
  Symbol symbol;                            /**< Instrument. */
  BookEventType type = BookEventType::kAdd; /**< The mutation kind. */
  OrderId order_id = 0;                     /**< Resting order (maker on trade).*/
  OrderId taker_id = 0;                     /**< Aggressor (kTrade only). */
  Side side = Side::Buy;                    /**< Order side / aggressor side. */
  Ticks price = 0;                          /**< Price level / execution / new
                                                 peg price. */
  Ticks prev_price = 0;                     /**< The old price (kReprice only). */
  Quantity qty = 0;                         /**< Leaves (add/cancel), fill
                                                 (trade), or new displayed slice
                                                 (replenish) -- the MATCHABLE
                                                 quantity. */
  Quantity displayed = 0;                   /**< The DISPLAYED (lit) portion: 0
                                                 for hidden, the slice for an
                                                 iceberg, else == qty. On a trade
                                                 it is the displayed amount the
                                                 fill consumed. Drives L1/L2. */
};

/**
 * @brief Consumer of the book-event stream (e.g. the feed-helper adapter).
 *
 * Called synchronously from the matcher during submit/cancel/expire/auction, in
 * mutation order. Implementations must not block or re-enter the book.
 */
class BookEventSink {
 public:
  virtual ~BookEventSink() = default;

  /** @brief Receive one sequence-numbered book event. */
  virtual void on_book_event(const BookEvent& ev) = 0;
};

}  // namespace codicis

#endif  // CODICIS_CORE_BOOK_EVENT_H
