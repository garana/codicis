/**
 * @file trading_engine.cc
 * @brief Implementation of the report-before-place TradingEngine.
 */

#include "codicis/engine/trading_engine.h"

#include <map>
#include <string>
#include <utility>

namespace codicis {
namespace {

/** @brief Side name for storage messages. */
const char* SideName(Side s) { return s == Side::Buy ? "buy" : "sell"; }

/** @brief Build the persisted fields for a new order. */
std::vector<std::pair<std::string, std::string>> OrderFields(
    const Order& o) {
  return {{"id", std::to_string(o.id)},
          {"side", SideName(o.side)},
          {"price", std::to_string(o.price)},
          {"qty", std::to_string(o.qty)}};
}

}  // namespace

OrderId TradingEngine::submit(Order order, SubmitCallback cb) {
  if (order.id == 0) {
    order.id = next_order_id_++;
  }
  const OrderId id = order.id;
  const SeqNo seq = next_assign_seq_++;

  Pending p;
  p.order = order;
  p.cb = std::move(cb);
  pending_.emplace(seq, std::move(p));

  // Report-before-place: only place once storage has acknowledged.
  storage_.report_order(OrderFields(order), [this, seq](bool ok) {
    const auto it = pending_.find(seq);
    if (it == pending_.end()) {
      return;
    }
    it->second.acked = true;
    it->second.ok = ok;
    drain();
  });
  return id;
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
    r.storage_ok = p.ok;
    if (p.ok) {
      r.outcome = book_.submit(p.order);
      report_results(p.order, r.outcome);
    }
    if (p.cb) {
      p.cb(r);
    }
  }
}

void TradingEngine::report_results(const Order& taker,
                                   const SubmitOutcome& out) {
  // Every execution is reported as a trade carrying both order ids.
  for (const Trade& t : out.trades) {
    storage_.report_trade({{"taker", std::to_string(t.taker_id)},
                           {"maker", std::to_string(t.maker_id)},
                           {"price", std::to_string(t.price)},
                           {"qty", std::to_string(t.qty)}},
                          nullptr);
  }

  // Report each affected order's fill (partial or complete). The taker's
  // fill comes from the outcome; each maker's remaining is read from the book
  // (absent => fully filled and removed).
  auto report_fill = [&](OrderId id, Quantity qty, Quantity remaining,
                         bool complete) {
    storage_.report_fill({{"id", std::to_string(id)},
                          {"qty", std::to_string(qty)},
                          {"remaining", std::to_string(remaining)},
                          {"status", complete ? "filled" : "partial"}},
                         nullptr);
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
    const Order* m = book_.find(mid);
    const Quantity remaining = m != nullptr ? m->leaves : 0;
    report_fill(mid, qty, remaining, m == nullptr);
  }
}

}  // namespace codicis
