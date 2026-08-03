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

/**
 * @brief Callback for a level pull.
 * @param ok    True if the pull succeeded.
 * @param reply The helper's response (valid when @p ok is true).
 */
using PullFn = std::function<void(bool ok, const HelperMessage& reply)>;

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
   * @brief Pull order-book levels not resident in memory.
   * @param symbol     The instrument symbol.
   * @param side       "buy" or "sell".
   * @param from_price The starting price (tick units) to pull from.
   * @param count      Number of levels to pull.
   * @param cb         Completion callback.
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
