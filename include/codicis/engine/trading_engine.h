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
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
   * @param matching   The per-symbol matching engine.
   * @param storage    The storage helper client.
   * @param mem_levels The resident-window bound per side (0 = unbounded, no
   *                   pull-back). Must match the value the matching engine's
   *                   books use; it is the pull batch size and warmup depth.
   */
  TradingEngine(MatchingEngine& matching, StorageClient& storage,
                std::size_t mem_levels = 0)
      : matching_(matching), storage_(storage), mem_levels_(mem_levels) {}

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

  /** @brief Callback delivering the executions from a session auction. */
  using AuctionCallback = std::function<void(const std::vector<Trade>&)>;

  /**
   * @brief Run a symbol's opening or closing uniform-price auction.
   *
   * Crosses the queued auction orders at a single clearing price, reports the
   * resulting trades and per-account fills to storage (best-effort, like a
   * continuous match), and delivers the executions. The queued orders were
   * already persisted when submitted, so nothing else is needed for durability.
   * @param symbol  The instrument.
   * @param opening True for the opening auction, false for the closing one.
   * @param cb      Receives the auction executions (invoked synchronously).
   */
  void run_auction(const Symbol& symbol, bool opening,
                   const AuctionCallback& cb);

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
    ClientId client_id = 0;   /**< Owner's client id (0 = anonymous). */
    bool acked = false;       /**< Storage acknowledged the pre-report. */
    bool ok = false;          /**< The pre-report succeeded. */
    bool pos_ready = false;   /**< The account's position is loaded. */
  };

  /** @brief Place all acknowledged orders that are next in sequence. */
  void drain();

  /**
   * @brief Ensure @p client's position for @p symbol is loaded from storage.
   *
   * On the account's first order for a symbol (when it has no book presence, so
   * no fill can race the seed) this pulls the authoritative net position from
   * the storage helper and seeds the book, then marks every pending order for
   * that account ready and drains. A no-op (returns ready) for an already
   * loaded account or an anonymous (client 0) order.
   * @param symbol The instrument.
   * @param owner  The owning user's UUID (for the storage query).
   * @param client The account (client id).
   * @return True if the position is already available (no pull needed).
   */
  bool ensure_position(const Symbol& symbol, const std::string& owner,
                       ClientId client);

  /** @brief Report an order's resulting trades and fills to storage. */
  void report_results(const Symbol& symbol, const Order& taker,
                      const SubmitOutcome& out);

  /** @brief Report a batch of auction trades and per-order fills to storage. */
  void report_auction(const Symbol& symbol, const std::vector<Trade>& trades);

  /** @brief Drop handles for makers removed by a match. */
  void prune_filled(const Symbol& symbol, const SubmitOutcome& out);

  /** @return The internal client id for an owner (0 for anonymous). */
  ClientId client_for(const std::string& owner);

  /**
   * @brief Ensure the resident window covers the head order before it places.
   *
   * With a bounded window, warms a symbol's top levels on first touch and pulls
   * the deep contra levels a crossing aggressor would reach, materializing them
   * so the synchronous matcher can see them. Issues async pulls and returns
   * false (the order stays parked) until the window covers it; the pull
   * completion resumes draining. Returns true immediately when unbounded or
   * already covered.
   * @param symbol The instrument.
   * @param order  The head order about to be placed.
   * @return True if the order may place now; false if a pull was issued.
   */
  bool ensure_depth(const Symbol& symbol, const Order& order);

  /** @brief Handle a pulled batch of deep orders for @p symbol on @p side. */
  void on_pull(const Symbol& symbol, Side side,
               std::vector<PulledOrder> orders);

  /** @return True if @p order would cross beyond the resident contra window. */
  bool needs_deep(const Symbol& symbol, const Order& order, Side contra) const;

  MatchingEngine& matching_;
  StorageClient& storage_;
  UuidGenerator uuids_;
  std::map<SeqNo, Pending> pending_;  // ordered by arrival seq

  std::unordered_map<std::string, Handle> handles_;   // order uuid -> handle
  std::unordered_map<OrderId, std::string> id_uuid_;  // internal id -> uuid
  std::unordered_map<std::string, ClientId> user_client_;  // owner -> client id

  // Per-account position load state, keyed by (symbol, client id): loaded once
  // the storage position is seeded; loading while a pull is in flight.
  std::set<std::pair<Symbol, ClientId>> position_loaded_;
  std::set<std::pair<Symbol, ClientId>> position_loading_;

  // Deep-level windowing (OT8). mem_levels_ == 0 disables it (unbounded books).
  std::size_t mem_levels_ = 0;
  std::set<Symbol> warmed_;         // symbols whose top-of-book was warmed
  int depth_pulls_ = 0;             // outstanding pull_levels requests
  // Orders cancelled while a pull was in flight: skip re-materializing them
  // from the (possibly stale) pull snapshot -- the cancel wins the race.
  std::set<OrderId> pull_ignore_;

  SeqNo next_assign_seq_ = 1;
  SeqNo next_place_seq_ = 1;
  OrderId next_order_id_ = 1;
  ClientId next_client_id_ = 1;
  std::size_t report_failures_ = 0;
};

}  // namespace codicis

#endif  // CODICIS_ENGINE_TRADING_ENGINE_H
