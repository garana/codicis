/**
 * @file trading_engine.cc
 * @brief Implementation of the report-before-place TradingEngine.
 */

#include "codicis/engine/trading_engine.h"

#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "codicis/util/logging.h"

namespace codicis {
namespace {

/** @brief Side name for storage messages. */
const char* SideName(Side s) { return s == Side::Buy ? "buy" : "sell"; }

/** @brief Build the persisted fields for a new order. */
std::vector<std::pair<std::string, std::string>> OrderFields(
    const Symbol& symbol, const std::string& owner, const Order& o) {
  return {{"symbol", symbol},
          {"owner", owner},  // lets storage attribute fills to an account
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
  const ClientId client_id = client_for(owner);
  order.client_id = client_id;
  const std::string order_uuid = uuids_.generate_string();
  const SeqNo seq = next_assign_seq_++;

  Pending p;
  p.symbol = symbol;
  p.owner = owner;
  p.order_uuid = order_uuid;
  p.order = order;
  p.cb = std::move(cb);
  p.client_id = client_id;
  // Placement waits on the account's position being loaded from storage (so
  // reduce-only clamps against the durable position). ensure_position() pulls
  // it on the account's first order for the symbol; it is already available for
  // anonymous or previously-loaded accounts.
  p.pos_ready = ensure_position(symbol, owner, client_id);
  pending_.emplace(seq, std::move(p));

  // Report-before-place: only place once storage has acknowledged.
  storage_.report_order(OrderFields(symbol, owner, order), [this, seq](bool ok) {
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

bool TradingEngine::ensure_position(const Symbol& symbol,
                                    const std::string& owner, ClientId client) {
  if (client == 0) {
    return true;  // anonymous: no position, nothing to load
  }
  const std::pair<Symbol, ClientId> key{symbol, client};
  if (position_loaded_.count(key) > 0) {
    return true;  // already seeded this session
  }
  if (position_loading_.count(key) > 0) {
    return false;  // a pull is already in flight; coalesce onto it
  }
  position_loading_.insert(key);
  // The account has no book presence before its first order places, so no fill
  // can change its position while this async pull is outstanding.
  storage_.pull_position(
      owner, symbol, [this, symbol, client](bool ok, std::int64_t net) {
        if (ok) {
          matching_.seed_position(symbol, client, net);
        }
        // Either way, stop gating on the pull: on failure the book stays flat
        // (reduce-only then conservatively finds nothing to reduce).
        const std::pair<Symbol, ClientId> k{symbol, client};
        position_loading_.erase(k);
        position_loaded_.insert(k);
        for (auto& [seq, pend] : pending_) {
          if (pend.client_id == client && pend.symbol == symbol) {
            pend.pos_ready = true;
          }
        }
        drain();
      });
  return false;
}

bool TradingEngine::needs_deep(const Symbol& symbol, const Order& order,
                               Side contra) const {
  if (!matching_.has_deep(symbol, contra)) {
    return false;
  }
  Ticks worst = 0;
  if (!matching_.worst_resident(symbol, contra, &worst)) {
    return true;  // no resident contra levels, but deep liquidity exists
  }
  if (order.type == OrdType::Market) {
    return true;  // a market order reaches as far as its quantity needs
  }
  // A limit reaches deep only if it crosses past the deepest resident contra.
  return order.side == Side::Buy ? order.price > worst : order.price < worst;
}

bool TradingEngine::ensure_depth(const Symbol& symbol, const Order& order) {
  if (mem_levels_ == 0) {
    return true;  // unbounded books hold everything; no pull-back
  }
  if (depth_pulls_ > 0) {
    return false;  // a pull is in flight; keep the head order parked
  }
  const auto batch = static_cast<std::int64_t>(mem_levels_);
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

  // Warm the top of book on the first order for a symbol (both sides), so the
  // resident window reflects storage before anything rests or matches.
  if (warmed_.find(symbol) == warmed_.end()) {
    warmed_.insert(symbol);
    depth_pulls_ += 2;
    storage_.pull_levels(symbol, "buy", kMax, batch,
                         [this, symbol](bool ok, std::vector<PulledOrder> os) {
                           on_pull(symbol, Side::Buy,
                                   ok ? std::move(os)
                                      : std::vector<PulledOrder>{});
                         });
    storage_.pull_levels(symbol, "sell", kMin, batch,
                         [this, symbol](bool ok, std::vector<PulledOrder> os) {
                           on_pull(symbol, Side::Sell,
                                   ok ? std::move(os)
                                      : std::vector<PulledOrder>{});
                         });
    return false;
  }

  // Pull the deep contra levels a crossing aggressor would reach.
  const Side contra = Opposite(order.side);
  if (needs_deep(symbol, order, contra)) {
    Ticks worst = 0;
    const std::int64_t from =
        matching_.worst_resident(symbol, contra, &worst)
            ? worst
            : (contra == Side::Sell ? kMin : kMax);
    depth_pulls_ += 1;
    storage_.pull_levels(symbol, SideName(contra), from, batch,
                         [this, symbol, contra](
                             bool ok, std::vector<PulledOrder> os) {
                           on_pull(symbol, contra,
                                   ok ? std::move(os)
                                      : std::vector<PulledOrder>{});
                         });
    return false;
  }
  return true;
}

void TradingEngine::on_pull(const Symbol& symbol, Side side,
                            std::vector<PulledOrder> orders) {
  for (const PulledOrder& po : orders) {
    if (pull_ignore_.count(po.id) > 0) {
      continue;  // cancelled while this pull was in flight
    }
    if (matching_.find(symbol, po.id) != nullptr) {
      continue;  // already resident (pulled by an earlier batch)
    }
    Order o;
    o.id = po.id;
    o.side = side;
    o.type = OrdType::Limit;
    o.price = po.price;
    o.qty = po.leaves;
    o.leaves = po.leaves;
    o.seq = po.seq;
    o.rank = po.seq;  // storage's ordering key is the priority rank
    matching_.insert_resident(symbol, o);
  }
  if (--depth_pulls_ == 0) {
    pull_ignore_.clear();
    drain();
  }
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
  const Symbol symbol = it->second.symbol;
  const OrderId id = it->second.id;
  // Remove from memory (a no-op if the order is deep/non-resident) AND from the
  // storage resting book (authoritative for deep orders that are not in memory).
  matching_.cancel(symbol, id);
  storage_.report_cancel(symbol, id, nullptr);
  report_requeued(symbol);  // cancelling a non-peg can reprice resting pegs
  // Race: if a deep pull is in flight it may carry a now-stale snapshot that
  // still contains this order; mark it so on_pull does not resurrect it.
  if (depth_pulls_ > 0) {
    pull_ignore_.insert(id);
  }
  id_uuid_.erase(id);
  handles_.erase(it);
  return CancelResult::kOk;
}

void TradingEngine::drain() {
  // Place acknowledged orders strictly in arrival order, so out-of-order acks
  // never reorder book placement. An order also waits until its account's
  // position has been loaded from storage (pos_ready).
  for (;;) {
    const auto it = pending_.find(next_place_seq_);
    if (it == pending_.end() || !it->second.acked || !it->second.pos_ready) {
      break;
    }
    // Ensure the resident window covers the head order's reach before placing
    // it; if a deep pull is issued, stop draining until it completes.
    if (!ensure_depth(it->second.symbol, it->second.order)) {
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
      // Register a handle if the order rests -- resident OR deep (both are
      // cancellable). A fully filled or discarded order has nothing to cancel.
      if (r.outcome.rested || r.outcome.rested_deep) {
        handles_.emplace(
            p.order_uuid,
            Handle{.symbol = p.symbol, .id = p.order.id, .owner = p.owner});
        id_uuid_.emplace(p.order.id, p.order_uuid);
        // Record it in the storage resting book so it can be pulled back later
        // (and so eviction/pull-back need no further storage write).
        const Order& ro = r.outcome.resting;
        storage_.report_rest(p.symbol, SideName(ro.side), ro.id, ro.price,
                             ro.leaves, ro.rank, nullptr);
      }
      report_results(p.symbol, p.order, r.outcome);
      report_requeued(p.symbol);  // pegs/icebergs that lost priority
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

void TradingEngine::report_requeued(const Symbol& symbol) {
  // Orders that moved to the back of a level get a fresh priority rank; update
  // their storage row (report_rest upserts by id) so pull_levels reconstructs
  // the correct queue order and price.
  for (const Order& o : matching_.take_requeued(symbol)) {
    storage_.report_rest(symbol, SideName(o.side), o.id, o.price, o.leaves,
                         o.rank, nullptr);
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

void TradingEngine::run_auction(const Symbol& symbol, bool opening,
                                const AuctionCallback& cb) {
  std::vector<Trade> trades = opening
                                  ? matching_.run_opening_auction(symbol)
                                  : matching_.run_closing_auction(symbol);
  report_auction(symbol, trades);
  report_requeued(symbol);  // auction fills can cascade stops and reprice pegs
  if (cb) {
    cb(trades);
  }
}

void TradingEngine::report_auction(const Symbol& symbol,
                                   const std::vector<Trade>& trades) {
  auto on_report = [this](bool ok) {
    if (!ok) {
      ++report_failures_;
      LogMessage(LogLevel::kError, "storage failed to record an auction trade");
    }
  };

  // Each execution is a trade carrying both order ids; storage attributes it to
  // both accounts' positions via the per-order fill reports below.
  std::map<OrderId, Quantity> filled;  // per-order executed qty (both sides)
  for (const Trade& t : trades) {
    storage_.report_trade({{"symbol", symbol},
                           {"taker", std::to_string(t.taker_id)},
                           {"maker", std::to_string(t.maker_id)},
                           {"price", std::to_string(t.price)},
                           {"qty", std::to_string(t.qty)}},
                          on_report);
    filled[t.taker_id] += t.qty;
    filled[t.maker_id] += t.qty;
  }
  for (const auto& [id, qty] : filled) {
    const Order* o = matching_.find(symbol, id);
    const Quantity remaining = o != nullptr ? o->leaves : 0;
    storage_.report_fill({{"symbol", symbol},
                          {"id", std::to_string(id)},
                          {"qty", std::to_string(qty)},
                          {"remaining", std::to_string(remaining)},
                          {"status", o == nullptr ? "filled" : "partial"}},
                         on_report);
  }
}

}  // namespace codicis
