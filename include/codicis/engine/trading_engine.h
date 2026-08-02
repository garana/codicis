#ifndef CODICIS_ENGINE_TRADING_ENGINE_H
#define CODICIS_ENGINE_TRADING_ENGINE_H

/**
 * @file trading_engine.h
 * @brief Deterministic report-before-place sequencing over the order book.
 *
 * TradingEngine sits between clients and the matching engine and enforces the
 * spec's storage ordering:
 *   1. Assign an arrival sequence number to each incoming order.
 *   2. Report the order to the storage helper FIRST.
 *   3. Only once storage acknowledges, place the order into the book.
 *   4. Report the resulting trades and per-order fills (partial or complete)
 *      to storage, then invoke the caller's completion callback.
 *
 * A staging buffer guarantees that placement happens in arrival-sequence order
 * even when storage acknowledgements return out of order, keeping matching
 * deterministic. All work is driven by the event loop via the async
 * @ref StorageClient; nothing blocks.
 *
 * Not yet handled here: pulling non-resident book levels on demand. That
 * requires the book to evict deep levels first (top-of-book windowing), which
 * is a separate feature; see CLAUDE.md.
 */

#include <cstdint>
#include <functional>
#include <map>

#include "codicis/core/order.h"
#include "codicis/core/order_book.h"
#include "codicis/core/types.h"
#include "codicis/ipc/storage_client.h"

namespace codicis {

/**
 * @brief Report-before-place coordinator around an OrderBook and StorageClient.
 */
class TradingEngine {
 public:
  /** @brief The result delivered once an order has been sequenced. */
  struct Result {
    bool storage_ok = false;  /**< False if the pre-report failed (not placed). */
    OrderId order_id = 0;     /**< The engine-assigned order id. */
    SubmitOutcome outcome;    /**< Matching result (valid when storage_ok). */
  };

  /** @brief Completion callback invoked after an order is placed (or fails). */
  using SubmitCallback = std::function<void(const Result&)>;

  /**
   * @brief Construct over a book and a storage client (both must outlive it).
   * @param book    The matching engine.
   * @param storage The storage helper client.
   */
  TradingEngine(OrderBook& book, StorageClient& storage)
      : book_(book), storage_(storage) {}

  /**
   * @brief Submit an order: report to storage, then place on acknowledgement.
   *
   * If @p order.id is 0 the engine assigns one. The callback fires after the
   * order is placed in arrival order (or immediately-in-order if its ack is
   * the next expected), with the matching outcome; or with storage_ok=false if
   * the pre-report failed, in which case the order is not placed.
   * @param order The order to submit.
   * @param cb    Completion callback (may be empty).
   * @return The order id assigned to (or carried by) the order.
   */
  OrderId submit(Order order, SubmitCallback cb);

  /**
   * @brief Ask storage to commit; committed entries leave the processed queue.
   */
  void commit() { storage_.commit(nullptr); }

  /** @return The number of orders reported but not yet placed. */
  std::size_t pending_placements() const { return pending_.size(); }

  /**
   * @return The number of trade/fill reports that failed to persist.
   *
   * These are reported after placement, so a failure means the book mutated
   * but storage did not record it. Currently surfaced (counted + logged) but
   * not yet compensated -- rollback is a deferred design item (see CLAUDE.md).
   */
  std::size_t report_failures() const { return report_failures_; }

 private:
  /** @brief A reported-but-not-yet-placed order awaiting in-order placement. */
  struct Pending {
    Order order;
    SubmitCallback cb;
    bool acked = false;
    bool ok = false;
  };

  /** @brief Place all acknowledged orders that are next in sequence. */
  void drain();

  /** @brief Report an order's resulting trades and fills to storage. */
  void report_results(const Order& taker, const SubmitOutcome& out);

  OrderBook& book_;
  StorageClient& storage_;
  std::map<SeqNo, Pending> pending_;  // ordered by arrival seq
  SeqNo next_assign_seq_ = 1;
  SeqNo next_place_seq_ = 1;
  OrderId next_order_id_ = 1;
  std::size_t report_failures_ = 0;
};

}  // namespace codicis

#endif  // CODICIS_ENGINE_TRADING_ENGINE_H
