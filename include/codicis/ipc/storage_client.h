#ifndef CODICIS_IPC_STORAGE_CLIENT_H
#define CODICIS_IPC_STORAGE_CLIENT_H

/**
 * @file storage_client.h
 * @brief The storage-helper schema layered on a HelperClient.
 *
 * StorageClient implements the persistence protocol from the spec: orders and
 * trades are *reported* to the helper and held in a "processed queue" (an
 * outbox) until a later commit is confirmed, at which point every entry up to
 * the confirmed watermark is removed. It can also pull order-book levels that
 * are not resident in memory.
 *
 * Rollback of the in-memory book on a storage error is intentionally deferred
 * (see the "To Design" queue in CLAUDE.md); this class only tracks outbox
 * state and surfaces success/failure to callers.
 */

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "codicis/ipc/helper_client.h"
#include "codicis/ipc/helper_message.h"

namespace codicis {

/**
 * @brief Callback for a report (order/trade) completion.
 * @param ok True if the helper acknowledged the report.
 */
using ReportFn = std::function<void(bool ok)>;

/**
 * @brief Callback for a commit completion.
 * @param ok        True if the commit was confirmed.
 * @param watermark The highest req_id the helper has committed.
 */
using CommitFn = std::function<void(bool ok, std::uint64_t watermark)>;

/** @brief One resting order returned by a deep-level pull. */
struct PulledOrder {
  std::uint64_t id = 0;      /**< Internal order id. */
  std::int64_t price = 0;    /**< Resting price (ticks). */
  std::int64_t leaves = 0;   /**< Remaining quantity. */
  std::uint64_t seq = 0;     /**< Arrival sequence (time priority). */
};

/**
 * @brief Callback for a deep-level pull.
 * @param ok     True if the pull succeeded.
 * @param orders The deep resting orders in range, best-price first then by seq.
 */
using PullFn = std::function<void(bool ok, std::vector<PulledOrder> orders)>;

/**
 * @brief Callback for a position pull.
 * @param ok  True if the query succeeded.
 * @param net The account's net position for the symbol (+long / -short).
 */
using PositionFn = std::function<void(bool ok, std::int64_t net)>;

/**
 * @brief Storage-protocol client with an outbox and commit watermark.
 */
class StorageClient {
 public:
  /**
   * @brief Construct over a helper connection.
   * @param helper The transport (must outlive this).
   */
  explicit StorageClient(HelperClient& helper) : helper_(helper) {}

  /**
   * @brief Report a new or updated order to storage.
   * @param fields The order fields to persist.
   * @param cb     Completion callback (may be empty).
   * @return The assigned req_id (also the outbox key).
   */
  std::uint64_t report_order(
      std::vector<std::pair<std::string, std::string>> fields, ReportFn cb);

  /**
   * @brief Report a trade to storage.
   * @param fields The trade fields to persist.
   * @param cb     Completion callback (may be empty).
   * @return The assigned req_id (also the outbox key).
   */
  std::uint64_t report_trade(
      std::vector<std::pair<std::string, std::string>> fields, ReportFn cb);

  /**
   * @brief Report an order fill (partial or complete) to storage.
   * @param fields The fill fields to persist.
   * @param cb     Completion callback (may be empty).
   * @return The assigned req_id (also the outbox key).
   */
  std::uint64_t report_fill(
      std::vector<std::pair<std::string, std::string>> fields, ReportFn cb);

  /**
   * @brief Ask the helper to commit; drop committed entries on confirmation.
   * @param cb Completion callback (may be empty).
   */
  void commit(CommitFn cb);

  /**
   * @brief Record a resting order that lives only in storage (deep, evicted or
   *        priced beyond the resident window), so it can be pulled back later.
   * @param symbol The instrument.
   * @param side   "buy" or "sell".
   * @param id     The internal order id.
   * @param price  The resting price (ticks).
   * @param leaves The remaining quantity.
   * @param seq    The arrival sequence (time priority).
   * @param cb     Completion callback (may be empty).
   */
  void report_deep(const std::string& symbol, const std::string& side,
                   std::uint64_t id, std::int64_t price, std::int64_t leaves,
                   std::uint64_t seq, ReportFn cb);

  /**
   * @brief Remove a deep resting order (it was cancelled or pulled resident).
   * @param symbol The instrument.
   * @param id     The internal order id.
   * @param cb     Completion callback (may be empty).
   */
  void remove_deep(const std::string& symbol, std::uint64_t id, ReportFn cb);

  /**
   * @brief Pull the best deep levels beyond @p from_price back into memory.
   * @param symbol     The instrument symbol.
   * @param side       "buy" or "sell".
   * @param from_price The resident boundary; deeper prices are returned.
   * @param count      Maximum number of price levels to pull.
   * @param cb         Completion callback with the resting orders in range.
   */
  void pull_levels(const std::string& symbol, const std::string& side,
                   std::int64_t from_price, std::int64_t count, PullFn cb);

  /**
   * @brief Pull an account's net position for a symbol from storage.
   *
   * Storage is the system of record for positions (derived from the reported
   * fill stream), so this returns the durable, authoritative net -- used to
   * seed the in-memory book for reduce-only clamping.
   * @param user   The owning user's UUID.
   * @param symbol The instrument symbol.
   * @param cb     Completion callback.
   */
  void pull_position(const std::string& user, const std::string& symbol,
                     PositionFn cb);

  /** @return The number of reported entries not yet committed. */
  std::size_t processed_pending() const { return outbox_.size(); }

 private:
  /** @brief State of an outbox entry. */
  enum class OutboxState { kPending, kAcked };

  /** @brief One reported order/trade awaiting commit. */
  struct OutboxEntry {
    std::string kind;  /**< "order" or "trade". */
    OutboxState state;
  };

  std::uint64_t report(const std::string& type, const std::string& kind,
                       std::vector<std::pair<std::string, std::string>> fields,
                       ReportFn cb);

  HelperClient& helper_;
  std::map<std::uint64_t, OutboxEntry> outbox_;
};

}  // namespace codicis

#endif  // CODICIS_IPC_STORAGE_CLIENT_H
