/**
 * @file order_book.cc
 * @brief Implementation of the dense-ladder order book and matcher.
 */

#include "codicis/core/order_book.h"

#include <algorithm>
#include <deque>
#include <list>
#include <unordered_map>

namespace codicis {
namespace {

/** @brief One price level: FIFO of resting order ids plus a running total. */
struct Level {
  Quantity total = 0;
  std::list<OrderId> fifo;
};

/**
 * @brief A dense price ladder for one side, indexed by tick offset.
 *
 * levels[i] corresponds to price (base + i). A std::deque gives O(1) random
 * access and O(1) growth at either end as the covered price range slides.
 */
struct Ladder {
  explicit Ladder(Side s) : side(s) {}

  Side side;
  std::deque<Level> levels;
  Ticks base = 0;
  bool has_base = false;
  bool best_valid = false;
  Ticks best_price = 0;

  /** @return True if price p is currently covered by the ladder. */
  bool covers(Ticks p) const {
    return has_base && p >= base &&
           p < base + static_cast<Ticks>(levels.size());
  }

  /** @return The deque index for a covered price. */
  std::size_t index(Ticks p) const {
    return static_cast<std::size_t>(p - base);
  }

  /** @return True if price a is a better (more aggressive) price than b. */
  bool better(Ticks a, Ticks b) const {
    return side == Side::Buy ? a > b : a < b;
  }

  /** @brief Ensure the ladder covers price p, growing as needed. */
  Level& touch(Ticks p) {
    if (!has_base) {
      base = p;
      levels.assign(1, Level{});
      has_base = true;
    } else if (p < base) {
      levels.insert(levels.begin(),
                    static_cast<std::size_t>(base - p), Level{});
      base = p;
    } else if (p >= base + static_cast<Ticks>(levels.size())) {
      levels.resize(static_cast<std::size_t>(p - base) + 1);
    }
    return levels[index(p)];
  }

  /** @return Pointer to the level at p, or nullptr if not covered. */
  Level* at(Ticks p) {
    if (!covers(p)) {
      return nullptr;
    }
    return &levels[index(p)];
  }

  /** @brief Record that quantity was added at price p. */
  void on_added(Ticks p) {
    if (best_valid) {
      if (better(p, best_price)) {
        best_price = p;
      }
    }
  }

  /** @brief Record that the level at p may now be empty. */
  void on_removed(Ticks p) {
    if (best_valid && p == best_price) {
      best_valid = false;
    }
  }

  /**
   * @brief Compute the best price on this side.
   * @param out Receives the best price on success.
   * @return True if any level has quantity.
   */
  bool best(Ticks* out) {
    if (best_valid) {
      *out = best_price;
      return true;
    }
    if (!has_base) {
      return false;
    }
    if (side == Side::Buy) {
      for (std::size_t i = levels.size(); i-- > 0;) {
        if (levels[i].total > 0) {
          best_price = base + static_cast<Ticks>(i);
          best_valid = true;
          *out = best_price;
          return true;
        }
      }
    } else {
      for (std::size_t i = 0; i < levels.size(); ++i) {
        if (levels[i].total > 0) {
          best_price = base + static_cast<Ticks>(i);
          best_valid = true;
          *out = best_price;
          return true;
        }
      }
    }
    return false;
  }
};

}  // namespace

/**
 * @brief Private state and matching logic for OrderBook.
 */
struct OrderBook::Impl {
  /** @brief A resting order plus its position in a level's FIFO. */
  struct Entry {
    Order order;
    std::list<OrderId>::iterator pos;
  };

  Ladder bids{Side::Buy};
  Ladder asks{Side::Sell};
  std::unordered_map<OrderId, Entry> orders;
  SeqNo next_seq = 1;
  StpPolicy stp = StpPolicy::kNone;

  /** @return The ladder for side s. */
  Ladder& ladder(Side s) { return s == Side::Buy ? bids : asks; }

  /** @return True if a marketable order o would cross the opposite side. */
  bool crosses(const Order& o) {
    Ladder& opp = ladder(Opposite(o.side));
    Ticks bp;
    if (!opp.best(&bp)) {
      return false;
    }
    if (o.type == OrdType::Market) {
      return true;
    }
    return o.side == Side::Buy ? o.price >= bp : o.price <= bp;
  }

  /**
   * @brief Maximum quantity of o that could fill immediately.
   * @param o The aggressor.
   * @return Total crossable resting quantity, capped at o.leaves.
   */
  Quantity available_fill(const Order& o) {
    Ladder& opp = ladder(Opposite(o.side));
    Quantity avail = 0;
    // Walk resident levels from best outward while the price still crosses.
    const std::size_t n = opp.levels.size();
    if (!opp.has_base) {
      return 0;
    }
    auto acceptable = [&](Ticks p) -> bool {
      if (o.type == OrdType::Market) {
        return true;
      }
      return o.side == Side::Buy ? o.price >= p : o.price <= p;
    };
    if (opp.side == Side::Sell) {  // asks ascending: iterate low->high
      for (std::size_t i = 0; i < n && avail < o.leaves; ++i) {
        const Ticks p = opp.base + static_cast<Ticks>(i);
        if (opp.levels[i].total > 0 && acceptable(p)) {
          avail += opp.levels[i].total;
        } else if (opp.levels[i].total > 0 && !acceptable(p)) {
          break;  // further levels are even less acceptable
        }
      }
    } else {  // bids descending: iterate high->low
      for (std::size_t i = n; i-- > 0 && avail < o.leaves;) {
        const Ticks p = opp.base + static_cast<Ticks>(i);
        if (opp.levels[i].total > 0 && acceptable(p)) {
          avail += opp.levels[i].total;
        } else if (opp.levels[i].total > 0 && !acceptable(p)) {
          break;
        }
      }
    }
    return std::min(avail, o.leaves);
  }

  /**
   * @brief Match aggressor o against the opposite side, appending trades.
   * @param o      The aggressor (mutated: leaves/filled).
   * @param trades Output executions.
   * @return True if self-trade prevention cancelled the aggressor remainder
   *         (so it must not rest).
   */
  bool match(Order& o, std::vector<Trade>& trades) {
    Ladder& opp = ladder(Opposite(o.side));
    while (o.leaves > 0) {
      Ticks bp;
      if (!opp.best(&bp)) {
        break;
      }
      if (o.type == OrdType::Limit) {
        const bool cross = o.side == Side::Buy ? o.price >= bp : o.price <= bp;
        if (!cross) {
          break;
        }
      }
      Level* lvl = opp.at(bp);
      while (o.leaves > 0 && lvl != nullptr && !lvl->fifo.empty()) {
        const OrderId mid = lvl->fifo.front();
        Entry& me = orders.at(mid);

        // Self-trade prevention: same non-zero account on both sides.
        if (stp != StpPolicy::kNone && o.client_id != 0 &&
            me.order.client_id == o.client_id) {
          const bool cancel_resting = stp == StpPolicy::kCancelResting ||
                                      stp == StpPolicy::kCancelBoth;
          const bool cancel_aggressor = stp == StpPolicy::kCancelAggressor ||
                                        stp == StpPolicy::kCancelBoth;
          if (cancel_resting) {
            lvl->total -= me.order.leaves;
            lvl->fifo.pop_front();
            orders.erase(mid);
          }
          if (cancel_aggressor) {
            if (lvl->total == 0) {
              opp.on_removed(bp);
            }
            return true;  // aggressor remainder cancelled; stop matching
          }
          continue;  // kCancelResting: maker gone, try the next maker
        }

        const Quantity fill = std::min(o.leaves, me.order.leaves);
        trades.push_back(Trade{o.id, mid, o.side, bp, fill});
        o.leaves -= fill;
        o.filled += fill;
        me.order.leaves -= fill;
        me.order.filled += fill;
        lvl->total -= fill;
        if (me.order.leaves == 0) {
          lvl->fifo.pop_front();
          orders.erase(mid);
        }
      }
      if (lvl != nullptr && lvl->total == 0) {
        opp.on_removed(bp);
      }
    }
    return false;
  }

  /**
   * @brief Rest the remainder of o on its own side.
   * @param o The order with leaves > 0.
   */
  void rest(const Order& o) {
    Ladder& own = ladder(o.side);
    Level& lvl = own.touch(o.price);
    lvl.fifo.push_back(o.id);
    auto it = std::prev(lvl.fifo.end());
    lvl.total += o.leaves;
    own.on_added(o.price);
    orders.emplace(o.id, Entry{o, it});
  }
};

OrderBook::OrderBook() : impl_(std::make_unique<Impl>()) {}

OrderBook::OrderBook(StpPolicy stp) : impl_(std::make_unique<Impl>()) {
  impl_->stp = stp;
}

OrderBook::~OrderBook() = default;

SubmitOutcome OrderBook::submit(Order order) {
  Normalize(&order);
  order.seq = impl_->next_seq++;

  SubmitOutcome out;
  out.order_id = order.id;

  if (order.qty <= 0 || order.leaves <= 0) {
    out.accepted = false;
    out.reject_reason = "non-positive quantity";
    return out;
  }
  if (order.type == OrdType::Limit && order.price <= 0) {
    out.accepted = false;
    out.reject_reason = "non-positive limit price";
    return out;
  }
  if (impl_->orders.find(order.id) != impl_->orders.end()) {
    out.accepted = false;
    out.reject_reason = "duplicate order id";
    return out;
  }

  // Post-Only must never take liquidity.
  if (HasFlag(order.flags, OrderFlag::kPostOnly) && impl_->crosses(order)) {
    out.accepted = false;
    out.reject_reason = "post-only would cross";
    return out;
  }

  // FOK / AON: all-or-none, so pre-scan before emitting any trade.
  if (HasFlag(order.flags, OrderFlag::kAon)) {
    if (impl_->available_fill(order) < order.leaves) {
      out.accepted = false;
      out.reject_reason = "all-or-none not fully fillable";
      return out;
    }
  }

  // Min-Quantity: require at least min_qty immediately available (partials
  // above the floor are allowed, unlike AON).
  if (order.min_qty > 0) {
    if (order.min_qty > order.qty) {
      out.accepted = false;
      out.reject_reason = "min_qty exceeds qty";
      return out;
    }
    if (impl_->available_fill(order) < order.min_qty) {
      out.accepted = false;
      out.reject_reason = "minimum quantity not available";
      return out;
    }
  }

  const bool aggressor_cancelled = impl_->match(order, out.trades);
  out.filled = order.filled;

  if (order.leaves > 0 && !aggressor_cancelled) {
    const bool discard =
        order.type == OrdType::Market || order.tif == Tif::IOC;
    if (!discard) {
      impl_->rest(order);
      out.rested = true;
    }
  }
  return out;
}

bool OrderBook::cancel(OrderId id) {
  const auto it = impl_->orders.find(id);
  if (it == impl_->orders.end()) {
    return false;
  }
  Impl::Entry& e = it->second;
  const Side side = e.order.side;
  const Ticks price = e.order.price;
  const Quantity leaves = e.order.leaves;
  Ladder& own = impl_->ladder(side);
  if (Level* lvl = own.at(price); lvl != nullptr) {
    lvl->total -= leaves;
    lvl->fifo.erase(e.pos);
    if (lvl->total == 0) {
      own.on_removed(price);
    }
  }
  impl_->orders.erase(it);
  return true;
}

const Order* OrderBook::find(OrderId id) const {
  const auto it = impl_->orders.find(id);
  return it == impl_->orders.end() ? nullptr : &it->second.order;
}

bool OrderBook::best_bid(Ticks* out) const { return impl_->bids.best(out); }
bool OrderBook::best_ask(Ticks* out) const { return impl_->asks.best(out); }

Quantity OrderBook::total_qty_at(Side side, Ticks price) const {
  Ladder& l = impl_->ladder(side);
  Level* lvl = l.at(price);
  return lvl == nullptr ? 0 : lvl->total;
}

std::vector<OrderId> OrderBook::expire(Timestamp now) {
  std::vector<OrderId> expired;
  for (const auto& kv : impl_->orders) {
    const Order& o = kv.second.order;
    if (o.expiry_ns > 0 && o.expiry_ns <= now) {
      expired.push_back(kv.first);
    }
  }
  std::sort(expired.begin(), expired.end());
  for (const OrderId id : expired) {
    cancel(id);
  }
  return expired;
}

std::size_t OrderBook::resting_count() const { return impl_->orders.size(); }

}  // namespace codicis
