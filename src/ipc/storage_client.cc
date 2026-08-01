/**
 * @file storage_client.cc
 * @brief Implementation of StorageClient (see storage_client.h).
 */

#include "codicis/ipc/storage_client.h"

#include <utility>

namespace codicis {

std::uint64_t StorageClient::report(
    const std::string& type, const std::string& kind,
    std::vector<std::pair<std::string, std::string>> fields, ReportFn cb) {
  // Reserve the outbox slot under the id the helper will echo. HelperClient
  // assigns ids sequentially, so send() returns the id we key on.
  const std::uint64_t id = helper_.send(
      type, std::move(fields),
      [this, cb = std::move(cb)](bool ok, const HelperMessage& reply) {
        const auto it = outbox_.find(reply.req_id);
        if (ok && it != outbox_.end()) {
          it->second.state = OutboxState::kAcked;
        }
        if (cb) {
          cb(ok);
        }
      });
  outbox_.emplace(id, OutboxEntry{kind, OutboxState::kPending});
  return id;
}

std::uint64_t StorageClient::report_order(
    std::vector<std::pair<std::string, std::string>> fields, ReportFn cb) {
  return report("report_order", "order", std::move(fields), std::move(cb));
}

std::uint64_t StorageClient::report_trade(
    std::vector<std::pair<std::string, std::string>> fields, ReportFn cb) {
  return report("report_trade", "trade", std::move(fields), std::move(cb));
}

std::uint64_t StorageClient::report_fill(
    std::vector<std::pair<std::string, std::string>> fields, ReportFn cb) {
  return report("report_fill", "fill", std::move(fields), std::move(cb));
}

void StorageClient::commit(CommitFn cb) {
  helper_.send(
      "commit", {},
      [this, cb = std::move(cb)](bool ok, const HelperMessage& reply) {
        std::uint64_t watermark = 0;
        if (ok) {
          if (const std::string* w = reply.get("committed")) {
            for (char c : *w) {
              if (c < '0' || c > '9') {
                break;
              }
              watermark =
                  watermark * 10 + static_cast<std::uint64_t>(c - '0');
            }
          }
          // Drop every outbox entry at or below the committed watermark.
          for (auto it = outbox_.begin();
               it != outbox_.end() && it->first <= watermark;) {
            it = outbox_.erase(it);
          }
        }
        if (cb) {
          cb(ok, watermark);
        }
      });
}

void StorageClient::pull_levels(const std::string& symbol,
                                const std::string& side,
                                std::int64_t from_price, std::int64_t count,
                                PullFn cb) {
  std::vector<std::pair<std::string, std::string>> fields = {
      {"symbol", symbol},
      {"side", side},
      {"from_price", std::to_string(from_price)},
      {"count", std::to_string(count)},
  };
  helper_.send("pull_levels", std::move(fields),
               [cb = std::move(cb)](bool ok, const HelperMessage& reply) {
                 if (cb) {
                   cb(ok, reply);
                 }
               });
}

}  // namespace codicis
