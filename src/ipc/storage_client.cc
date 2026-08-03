/**
 * @file storage_client.cc
 * @brief Implementation of StorageClient (see storage_client.h).
 */

#include "codicis/ipc/storage_client.h"

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

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

namespace {

/** @brief Parse a decimal integer (optionally signed); 0 on non-digits. */
std::int64_t ParseI64(std::string_view s) {
  std::int64_t v = 0;
  std::size_t i = 0;
  bool neg = false;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    i = 1;
  }
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') {
      break;
    }
    v = v * 10 + (s[i] - '0');
  }
  return neg ? -v : v;
}

/**
 * @brief Parse the "orders" blob: records "id,price,leaves,seq" joined by ';'.
 * @param blob The encoded order list.
 * @return The decoded resting orders.
 */
std::vector<PulledOrder> ParseOrders(const std::string& blob) {
  std::vector<PulledOrder> out;
  std::size_t pos = 0;
  while (pos < blob.size()) {
    const std::size_t end = blob.find(';', pos);
    const std::string_view rec(blob.data() + pos,
                               (end == std::string::npos ? blob.size() : end) -
                                   pos);
    // Split rec into up to four comma-separated fields.
    std::int64_t f[4] = {0, 0, 0, 0};
    std::size_t fi = 0;
    std::size_t fp = 0;
    while (fi < 4 && fp <= rec.size()) {
      const std::size_t fe = rec.find(',', fp);
      const std::string_view fld(
          rec.data() + fp, (fe == std::string_view::npos ? rec.size() : fe) - fp);
      f[fi++] = ParseI64(fld);
      if (fe == std::string_view::npos) {
        break;
      }
      fp = fe + 1;
    }
    if (!rec.empty()) {
      out.push_back(PulledOrder{static_cast<std::uint64_t>(f[0]), f[1], f[2],
                                static_cast<std::uint64_t>(f[3])});
    }
    if (end == std::string::npos) {
      break;
    }
    pos = end + 1;
  }
  return out;
}

}  // namespace

void StorageClient::report_deep(const std::string& symbol,
                                const std::string& side, std::uint64_t id,
                                std::int64_t price, std::int64_t leaves,
                                std::uint64_t seq, ReportFn cb) {
  helper_.send("report_deep",
               {{"symbol", symbol},
                {"side", side},
                {"id", std::to_string(id)},
                {"price", std::to_string(price)},
                {"leaves", std::to_string(leaves)},
                {"seq", std::to_string(seq)}},
               [cb = std::move(cb)](bool ok, const HelperMessage&) {
                 if (cb) {
                   cb(ok);
                 }
               });
}

void StorageClient::remove_deep(const std::string& symbol, std::uint64_t id,
                                ReportFn cb) {
  helper_.send("remove_deep",
               {{"symbol", symbol}, {"id", std::to_string(id)}},
               [cb = std::move(cb)](bool ok, const HelperMessage&) {
                 if (cb) {
                   cb(ok);
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
                 std::vector<PulledOrder> orders;
                 if (ok) {
                   if (const std::string* blob = reply.get("orders")) {
                     orders = ParseOrders(*blob);
                   }
                 }
                 if (cb) {
                   cb(ok, std::move(orders));
                 }
               });
}

void StorageClient::pull_position(const std::string& user,
                                  const std::string& symbol, PositionFn cb) {
  helper_.send("pull_position", {{"user", user}, {"symbol", symbol}},
               [cb = std::move(cb)](bool ok, const HelperMessage& reply) {
                 std::int64_t net = 0;
                 if (ok) {
                   if (const std::string* s = reply.get("net")) {
                     bool neg = false;
                     std::size_t i = 0;
                     if (!s->empty() && ((*s)[0] == '-' || (*s)[0] == '+')) {
                       neg = (*s)[0] == '-';
                       i = 1;
                     }
                     for (; i < s->size(); ++i) {
                       const char c = (*s)[i];
                       if (c < '0' || c > '9') {
                         break;
                       }
                       net = net * 10 + (c - '0');
                     }
                     if (neg) {
                       net = -net;
                     }
                   }
                 }
                 if (cb) {
                   cb(ok, net);
                 }
               });
}

}  // namespace codicis
