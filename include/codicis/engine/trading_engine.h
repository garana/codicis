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
#include <string>
#include <unordered_map>

#include "codicis/core/matching_engine.h"
#include "codicis/core/order.h"
#include "codicis/core/order_book.h"
#include "codicis/core/types.h"
#include "codicis/ipc/storage_client.h"
#include "codicis/util/uuid.h"

namespace codicis {

/** @brief Outcome of an authorized cancel-by-handle. */
enum class CancelResult {
  kOk,         /**< The order was cancelled. */
  kNotFound,   /**< No resting order with that handle. */
  kForbidden,  /**< The requester is not the order's owner. */
};

/**
 * @brief Report-before-place coordinator around a MatchingEngine + storage.
 *
 * Assigns each order an external, opaque UUID handle (returned to the client),
 * mapped internally to (symbol, integer id). Each order has an owner (a user
 * UUID); a cancel is authorized only if the requester matches the owner. The
 * owner also maps to an internal client id so self-trade prevention is keyed on
 * the same account.
 */
class TradingEngine {
 public:
  /** @brief The result delivered once an order has been sequenced. */
  struct Result {
    bool storage_ok = false;  /**< False if the pre-report failed (not placed). */
    OrderId order_id = 0;     /**< The internal order id. */
    std::string order_uuid;   /**< The external order handle. */
    SubmitOutcome outcome;    /**< Matching result (valid when storage_ok). */
  };

  /** @brief Completion callback invoked after an order is placed (or fails). */
  using SubmitCallback = std::function<void(const Result&)>;

  /**
   * @brief Construct over a matching engine and storage client (both must
   *        outlive it).
   * @param matching The per-symbol matching engine.
   * @param storage  The storage helper client.
   */
  TradingEngine(MatchingEngine& matching, StorageClient& storage)
      : matching_(matching), storage_(storage) {}

  /**
   * @brief Submit an order to a symbol on behalf of an owner.
   *
   * The engine assigns an external UUID handle and an internal id, resolves the
   * owner to a client id (for self-trade prevention), reports the order to
   * storage, and only then places it (in arrival order). The callback receives
   * the UUID handle and matching outcome, or storage_ok=false if the pre-report
   * failed (in which case the order is not placed).
   * @param symbol The instrument to submit to.
   * @param owner  The owning user's UUID (empty for anonymous).
   * @param order  The order to submit (id assigned by the engine).
   * @param cb     Completion callback (may be empty).
   * @return The external UUID handle assigned to the order.
   */
  std::string submit(const Symbol& symbol, const std::string& owner,
                     Order order, SubmitCallback cb);

  /**
   * @brief Cancel a resting order by its UUID handle, if the requester owns it.
   * @param order_uuid  The external order handle.
   * @param requester   The requesting user's UUID.
   * @param symbol_out  If non-null and the result is kOk, receives the symbol
   *                    whose book changed (for a market-data broadcast).
   * @return kOk, kNotFound (unknown handle), or kForbidden (not the owner).
   */
  CancelResult cancel(const std::string& order_uuid,
                      const std::string& requester,
                      Symbol* symbol_out = nullptr);

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
  /** @brief A resting order's handle: where it lives and who owns it. */
  struct Handle {
    Symbol symbol;
    OrderId id;
    std::string owner;
  };

  /** @brief A reported-but-not-yet-placed order awaiting in-order placement. */
  struct Pending {
    Symbol symbol;
    std::string owner;
    std::string order_uuid;
    Order order;
    SubmitCallback cb;
    bool acked = false;
    bool ok = false;
  };

  /** @brief Place all acknowledged orders that are next in sequence. */
  void drain();

  /** @brief Report an order's resulting trades and fills to storage. */
  void report_results(const Symbol& symbol, const Order& taker,
                      const SubmitOutcome& out);

  /** @brief Drop handles for makers removed by a match. */
  void prune_filled(const Symbol& symbol, const SubmitOutcome& out);

  /** @return The internal client id for an owner (0 for anonymous). */
  ClientId client_for(const std::string& owner);

  MatchingEngine& matching_;
  StorageClient& storage_;
  UuidGenerator uuids_;
  std::map<SeqNo, Pending> pending_;  // ordered by arrival seq

  std::unordered_map<std::string, Handle> handles_;   // order uuid -> handle
  std::unordered_map<OrderId, std::string> id_uuid_;  // internal id -> uuid
  std::unordered_map<std::string, ClientId> user_client_;  // owner -> client id

  SeqNo next_assign_seq_ = 1;
  SeqNo next_place_seq_ = 1;
  OrderId next_order_id_ = 1;
  ClientId next_client_id_ = 1;
  std::size_t report_failures_ = 0;
};

}  // namespace codicis

#endif  // CODICIS_ENGINE_TRADING_ENGINE_H
