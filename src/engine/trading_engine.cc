/**
 * @file trading_engine.cc
 * @brief Implementation of the report-before-place TradingEngine.
 */

#include "codicis/engine/trading_engine.h"

#include <map>
#include <string>
#include <utility>

#include "codicis/util/logging.h"

namespace codicis {
namespace {

/** @brief Side name for storage messages. */
const char* SideName(Side s) { return s == Side::Buy ? "buy" : "sell"; }

/** @brief Build the persisted fields for a new order. */
std::vector<std::pair<std::string, std::string>> OrderFields(
    const Symbol& symbol, const Order& o) {
  return {{"symbol", symbol},
          {"id", std::to_string(o.id)},
          {"side", SideName(o.side)},
          {"price", std::to_string(o.price)},
          {"qty", std::to_string(o.qty)}};
}

}  // namespace

ClientId TradingEngine::client_for(const std::string& owner) {
  if (owner.empty()) {
    return 0;  // anonymous: self-trade prevention disabled
  }
  const auto it = user_client_.find(owner);
  if (it != user_client_.end()) {
    return it->second;
  }
  const ClientId cid = next_client_id_++;
  user_client_.emplace(owner, cid);
  return cid;
}

std::string TradingEngine::submit(const Symbol& symbol,
                                  const std::string& owner, Order order,
                                  SubmitCallback cb) {
  if (order.id == 0) {
    order.id = next_order_id_++;
  }
  order.client_id = client_for(owner);
  const std::string order_uuid = uuids_.generate_string();
  const SeqNo seq = next_assign_seq_++;

  Pending p;
  p.symbol = symbol;
  p.owner = owner;
  p.order_uuid = order_uuid;
  p.order = order;
  p.cb = std::move(cb);
  pending_.emplace(seq, std::move(p));

  // Report-before-place: only place once storage has acknowledged.
  storage_.report_order(OrderFields(symbol, order), [this, seq](bool ok) {
    const auto it = pending_.find(seq);
    if (it == pending_.end()) {
      return;
    }
    it->second.acked = true;
    it->second.ok = ok;
    drain();
  });
  return order_uuid;
}

CancelResult TradingEngine::cancel(const std::string& order_uuid,
                                   const std::string& requester,
                                   Symbol* symbol_out) {
  const auto it = handles_.find(order_uuid);
  if (it == handles_.end()) {
    return CancelResult::kNotFound;
  }
  if (it->second.owner != requester) {
    return CancelResult::kForbidden;  // only the owner may cancel
  }
  if (symbol_out != nullptr) {
    *symbol_out = it->second.symbol;
  }
  matching_.cancel(it->second.symbol, it->second.id);
  id_uuid_.erase(it->second.id);
  handles_.erase(it);
  return CancelResult::kOk;
}

void TradingEngine::drain() {
  // Place acknowledged orders strictly in arrival order, so out-of-order acks
  // never reorder book placement.
  for (;;) {
    const auto it = pending_.find(next_place_seq_);
    if (it == pending_.end() || !it->second.acked) {
      break;
    }
    Pending p = std::move(it->second);
    pending_.erase(it);
    ++next_place_seq_;

    Result r;
    r.order_id = p.order.id;
    r.order_uuid = p.order_uuid;
    r.storage_ok = p.ok;
    if (p.ok) {
      r.outcome = matching_.submit(p.symbol, p.order);
      // Register the handle only if the order rests (a fully filled or
      // discarded order has nothing to cancel later).
      if (r.outcome.rested) {
        handles_.emplace(
            p.order_uuid,
            Handle{.symbol = p.symbol, .id = p.order.id, .owner = p.owner});
        id_uuid_.emplace(p.order.id, p.order_uuid);
      }
      report_results(p.symbol, p.order, r.outcome);
      prune_filled(p.symbol, r.outcome);
    }
    if (p.cb) {
      p.cb(r);
    }
  }
}

void TradingEngine::prune_filled(const Symbol& symbol,
                                 const SubmitOutcome& out) {
  // A resting maker that was fully filled is gone; drop its handle.
  for (const Trade& t : out.trades) {
    if (matching_.find(symbol, t.maker_id) == nullptr) {
      const auto it = id_uuid_.find(t.maker_id);
      if (it != id_uuid_.end()) {
        handles_.erase(it->second);
        id_uuid_.erase(it);
      }
    }
  }
}

void TradingEngine::report_results(const Symbol& symbol, const Order& taker,
                                   const SubmitOutcome& out) {
  // A trade/fill report failing means the book mutated but storage did not
  // record it. Surface it (count + log); compensation/rollback is deferred.
  auto on_report = [this](bool ok) {
    if (!ok) {
      ++report_failures_;
      LogMessage(LogLevel::kError, "storage failed to record a trade/fill");
    }
  };

  // Every execution is reported as a trade carrying both order ids.
  for (const Trade& t : out.trades) {
    storage_.report_trade({{"symbol", symbol},
                           {"taker", std::to_string(t.taker_id)},
                           {"maker", std::to_string(t.maker_id)},
                           {"price", std::to_string(t.price)},
                           {"qty", std::to_string(t.qty)}},
                          on_report);
  }

  // Report each affected order's fill (partial or complete). The taker's
  // fill comes from the outcome; each maker's remaining is read from the book
  // (absent => fully filled and removed).
  auto report_fill = [&](OrderId id, Quantity qty, Quantity remaining,
                         bool complete) {
    storage_.report_fill({{"symbol", symbol},
                          {"id", std::to_string(id)},
                          {"qty", std::to_string(qty)},
                          {"remaining", std::to_string(remaining)},
                          {"status", complete ? "filled" : "partial"}},
                         on_report);
  };

  if (out.filled > 0) {
    const Quantity remaining = taker.qty - out.filled;
    report_fill(taker.id, out.filled, remaining, remaining == 0);
  }

  std::map<OrderId, Quantity> maker_filled;
  for (const Trade& t : out.trades) {
    maker_filled[t.maker_id] += t.qty;
  }
  for (const auto& [mid, qty] : maker_filled) {
    const Order* m = matching_.find(symbol, mid);
    const Quantity remaining = m != nullptr ? m->leaves : 0;
    report_fill(mid, qty, remaining, m == nullptr);
  }
}

}  // namespace codicis
