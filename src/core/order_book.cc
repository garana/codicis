/**
 * @file order_book.cc
 * @brief Implementation of the dense-ladder order book and matcher.
 */

#include "codicis/core/order_book.h"

#include <algorithm>
#include <deque>
#include <list>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace codicis {
namespace {

/** @brief One price level: FIFO of resting order ids plus running totals. */
struct Level {
  Quantity total = 0;      /**< Matchable quantity (incl. hidden/reserve). */
  Quantity displayed = 0;  /**< Publicly visible quantity. */
  std::list<OrderId> fifo;
};

/**
 * @brief The displayed contribution of a resting order.
 * @param flags  The order's flag bitset.
 * @param leaves Remaining quantity.
 * @param slice  Current iceberg visible slice (ignored unless iceberg).
 * @return Quantity contributed to the public book view.
 */
Quantity DisplayedOf(std::uint32_t flags, Quantity leaves, Quantity slice) {
  if (HasFlag(flags, OrderFlag::kHidden)) {
    return 0;
  }
  if (HasFlag(flags, OrderFlag::kIceberg)) {
    return slice;
  }
  return leaves;
}

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
    Quantity slice = 0;  /**< Iceberg visible slice remaining (else unused). */
  };

  /** @brief A contingent-order group (OCO / OTO / bracket). */
  struct Group {
    OrderId parent = 0;               /**< OTO/bracket parent order id. */
    std::vector<OrderId> oco_legs;    /**< Active mutually-cancelling legs. */
    std::optional<Order> pending_child;  /**< OTO child, released on fill. */
    std::vector<Order> pending_oco;   /**< Bracket TP/SL, released as OCO. */
  };

  Ladder bids{Side::Buy};
  Ladder asks{Side::Sell};
  std::unordered_map<OrderId, Entry> orders;
  std::unordered_map<OrderId, Order> stops;  // parked, awaiting a trigger
  std::unordered_set<OrderId> pegged;        // resting orders that reprice
  std::unordered_map<std::uint64_t, Group> groups;   // contingent groups
  std::unordered_map<OrderId, std::uint64_t> order_group;  // linked -> group
  SeqNo next_seq = 1;
  StpPolicy stp = StpPolicy::kNone;
  bool have_last = false;
  Ticks last_price = 0;

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
   * @brief Match aggressor o against one price level's FIFO.
   *
   * Iterates the queue rather than only its head so it can: skip a resting
   * all-or-none maker that o cannot fully fill (a deliberate FIFO priority
   * inversion -- orders behind it may still fill); re-queue an exhausted
   * iceberg slice at the back; and apply self-trade prevention. A pass that
   * fills nothing (only unfillable AON makers remain) ends the level.
   * @param o      The aggressor (mutated).
   * @param opp    The opposite ladder (for best-cache invalidation).
   * @param lvl    The level being matched.
   * @param p      The level's price.
   * @param trades Output executions (appended).
   * @return True if self-trade prevention cancelled the aggressor remainder.
   */
  bool match_level(Order& o, Ladder& opp, Level& lvl, Ticks p,
                   std::vector<Trade>& trades) {
    auto it = lvl.fifo.begin();
    bool progressed = false;
    while (o.leaves > 0) {
      if (it == lvl.fifo.end()) {
        if (!progressed) {
          break;  // a full pass filled nothing: only unfillable AON remains
        }
        it = lvl.fifo.begin();
        progressed = false;
        continue;
      }
      const OrderId mid = *it;
      Entry& me = orders.at(mid);

      // Self-trade prevention: same non-zero account on both sides.
      if (stp != StpPolicy::kNone && o.client_id != 0 &&
          me.order.client_id == o.client_id) {
        const bool cancel_resting = stp == StpPolicy::kCancelResting ||
                                    stp == StpPolicy::kCancelBoth;
        const bool cancel_aggressor = stp == StpPolicy::kCancelAggressor ||
                                      stp == StpPolicy::kCancelBoth;
        if (cancel_resting) {
          lvl.total -= me.order.leaves;
          lvl.displayed -=
              DisplayedOf(me.order.flags, me.order.leaves, me.slice);
          it = lvl.fifo.erase(it);
          orders.erase(mid);
          pegged.erase(mid);
          progressed = true;
        }
        if (cancel_aggressor) {
          if (lvl.total == 0) {
            opp.on_removed(p);
          }
          return true;  // aggressor remainder cancelled; stop matching
        }
        continue;  // kCancelResting: maker gone, try the next maker
      }

      // Maker-side all-or-none: fillable only in full, else skip it.
      if (HasFlag(me.order.flags, OrderFlag::kAon) &&
          o.leaves < me.order.leaves) {
        ++it;
        continue;
      }

      const bool iceberg = HasFlag(me.order.flags, OrderFlag::kIceberg);
      const bool hidden = HasFlag(me.order.flags, OrderFlag::kHidden);
      // An iceberg exposes only its current slice to a single fill; other
      // orders expose their full remainder.
      const Quantity avail = iceberg ? me.slice : me.order.leaves;
      const Quantity fill = std::min(o.leaves, avail);
      trades.push_back(Trade{.taker_id = o.id,
                             .maker_id = mid,
                             .taker_side = o.side,
                             .price = p,
                             .qty = fill});
      last_price = p;  // stop-trigger reference
      have_last = true;
      o.leaves -= fill;
      o.filled += fill;
      me.order.leaves -= fill;
      me.order.filled += fill;
      lvl.total -= fill;
      if (!hidden) {
        lvl.displayed -= fill;  // displayed tracks leaves (normal) / slice
      }
      if (iceberg) {
        me.slice -= fill;
      }

      if (me.order.leaves == 0) {
        it = lvl.fifo.erase(it);
        orders.erase(mid);
        pegged.erase(mid);  // no-op unless it was a pegged order
        progressed = true;
      } else if (iceberg && me.slice == 0) {
        // Slice exhausted: replenish from the reserve and re-queue at the back
        // of the level (losing time priority); continue with the next order.
        const auto next = std::next(it);
        lvl.fifo.splice(lvl.fifo.end(), lvl.fifo, it);
        me.pos = std::prev(lvl.fifo.end());
        me.slice = std::min(me.order.display_qty, me.order.leaves);
        lvl.displayed += me.slice;
        it = next;
        progressed = true;
      } else {
        // Partial fill of a non-iceberg maker: the aggressor is now exhausted.
        progressed = true;
        ++it;
      }
    }
    if (lvl.total == 0) {
      opp.on_removed(p);
    }
    return false;
  }

  /**
   * @brief Match aggressor o against the opposite side, best price first.
   * @param o      The aggressor (mutated: leaves/filled).
   * @param trades Output executions.
   * @return True if self-trade prevention cancelled the aggressor remainder.
   */
  bool match(Order& o, std::vector<Trade>& trades) {
    Ladder& opp = ladder(Opposite(o.side));
    if (!opp.has_base) {
      return false;
    }
    auto acceptable = [&](Ticks p) {
      if (o.type == OrdType::Market) {
        return true;
      }
      return o.side == Side::Buy ? o.price >= p : o.price <= p;
    };
    const std::size_t n = opp.levels.size();
    // Walk resident levels from best outward while the price still crosses. A
    // level yielding no fill (e.g. only unfillable AON makers) is stepped over.
    if (opp.side == Side::Sell) {  // asks: lowest price is best
      for (std::size_t i = 0; i < n && o.leaves > 0; ++i) {
        const Ticks p = opp.base + static_cast<Ticks>(i);
        if (opp.levels[i].total == 0) {
          continue;
        }
        if (!acceptable(p)) {
          break;
        }
        if (match_level(o, opp, opp.levels[i], p, trades)) {
          return true;
        }
      }
    } else {  // bids: highest price is best
      for (std::size_t i = n; i-- > 0 && o.leaves > 0;) {
        const Ticks p = opp.base + static_cast<Ticks>(i);
        if (opp.levels[i].total == 0) {
          continue;
        }
        if (!acceptable(p)) {
          break;
        }
        if (match_level(o, opp, opp.levels[i], p, trades)) {
          return true;
        }
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
    const Quantity slice = HasFlag(o.flags, OrderFlag::kIceberg)
                               ? std::min(o.display_qty, o.leaves)
                               : 0;
    lvl.total += o.leaves;
    lvl.displayed += DisplayedOf(o.flags, o.leaves, slice);
    own.on_added(o.price);
    orders.emplace(o.id, Entry{.order = o, .pos = it, .slice = slice});
  }

  /**
   * @brief Whether a stop's trigger condition is met at price @p px.
   *
   * A buy stop sits above the market and fires as price rises to it; a sell
   * stop sits below and fires as price falls to it.
   * @param o  The stop order (o.trigger must be set).
   * @param px The reference (last trade) price.
   * @return True if the stop should fire.
   */
  bool stop_triggered(const Order& o, Ticks px) const {
    if (!o.trigger.has_value()) {
      return false;
    }
    const Ticks sp = o.trigger->stop_price;
    return o.side == Side::Buy ? px >= sp : px <= sp;
  }

  /**
   * @brief Initialize a trailing stop's high-water mark and stop price.
   * @param o The stop order (mutated).
   */
  void arm_stop(Order& o) {
    if (!o.trigger.has_value()) {
      return;
    }
    TriggerSpec& t = *o.trigger;
    if (t.kind == TriggerSpec::Kind::Stop) {
      return;  // fixed stop price, nothing to seed
    }
    const Ticks ref = have_last ? last_price : t.stop_price;
    t.high_water = ref;
    // Sell stop trails below the peak; buy stop trails above the trough.
    t.stop_price = o.side == Side::Sell ? ref - t.trail_by : ref + t.trail_by;
  }

  /**
   * @brief Re-anchor a trailing stop after a favorable price move.
   * @param o  The stop order (mutated).
   * @param px The new reference price.
   */
  void update_trail(Order& o, Ticks px) {
    if (!o.trigger.has_value()) {
      return;
    }
    TriggerSpec& t = *o.trigger;
    if (t.kind == TriggerSpec::Kind::Stop) {
      return;
    }
    if (o.side == Side::Sell) {
      if (px > t.high_water) {
        t.high_water = px;
        t.stop_price = px - t.trail_by;
      }
    } else {
      if (px < t.high_water) {
        t.high_water = px;
        t.stop_price = px + t.trail_by;
      }
    }
  }

  /**
   * @brief Inject a triggered stop as a plain market/limit order.
   * @param o      The now-active order (mutated).
   * @param trades Output executions (appended).
   */
  void inject(Order& o, std::vector<Trade>& trades) {
    match(o, trades);
    if (o.leaves > 0 && o.type != OrdType::Market && o.tif != Tif::IOC) {
      rest(o);
    }
  }

  /**
   * @brief Fire every stop whose trigger is met, cascading until quiescent.
   *
   * A triggered order can move the last price and set off further stops, so we
   * loop. Firing order within a round is by arrival sequence for determinism.
   * @param trades Output executions (appended).
   */
  void evaluate_stops(std::vector<Trade>& trades) {
    for (;;) {
      std::vector<OrderId> fire;
      for (auto& [id, so] : stops) {
        if (have_last) {
          update_trail(so, last_price);
        }
        if (have_last && stop_triggered(so, last_price)) {
          fire.push_back(id);
        }
      }
      if (fire.empty()) {
        return;
      }
      std::sort(fire.begin(), fire.end(),
                [&](OrderId a, OrderId b) {
                  return stops.at(a).seq < stops.at(b).seq;
                });
      for (const OrderId id : fire) {
        Order o = stops.at(id);
        stops.erase(id);
        if (o.trigger.has_value()) {
          o.trigger->triggered = true;
        }
        inject(o, trades);
      }
    }
  }

  /**
   * @brief Best price on a side considering only non-pegged orders.
   *
   * Pegs reference this so they never chase their own quotes.
   * @param side The side to inspect.
   * @param out  Receives the best non-pegged price on success.
   * @return True if a non-pegged order rests on that side.
   */
  bool reference_best(Side side, Ticks* out) {
    Ladder& L = ladder(side);
    if (!L.has_base) {
      return false;
    }
    auto has_nonpeg = [&](const Level& lvl) {
      for (const OrderId id : lvl.fifo) {
        const auto it = orders.find(id);
        if (it != orders.end() && !it->second.order.peg.has_value()) {
          return true;
        }
      }
      return false;
    };
    const std::size_t n = L.levels.size();
    if (side == Side::Buy) {
      for (std::size_t i = n; i-- > 0;) {
        if (L.levels[i].total > 0 && has_nonpeg(L.levels[i])) {
          *out = L.base + static_cast<Ticks>(i);
          return true;
        }
      }
    } else {
      for (std::size_t i = 0; i < n; ++i) {
        if (L.levels[i].total > 0 && has_nonpeg(L.levels[i])) {
          *out = L.base + static_cast<Ticks>(i);
          return true;
        }
      }
    }
    return false;
  }

  /**
   * @brief Compute a pegged order's price from the reference BBO.
   * @param o   The pegged order (o.peg must be set).
   * @param out Receives the computed price on success.
   * @return True if a valid price could be computed (reference available).
   */
  bool compute_peg_price(const Order& o, Ticks* out) {
    if (!o.peg.has_value()) {
      return false;
    }
    Ticks rb = 0;
    Ticks ra = 0;
    const bool hb = reference_best(Side::Buy, &rb);
    const bool ha = reference_best(Side::Sell, &ra);
    const PegSpec& p = *o.peg;
    Ticks ref = 0;
    switch (p.ref) {
      case PegSpec::Ref::Primary:
        if (o.side == Side::Buy) {
          if (!hb) return false;
          ref = rb;
        } else {
          if (!ha) return false;
          ref = ra;
        }
        break;
      case PegSpec::Ref::Market:
        if (o.side == Side::Buy) {
          if (!ha) return false;
          ref = ra;
        } else {
          if (!hb) return false;
          ref = rb;
        }
        break;
      case PegSpec::Ref::Midpoint:
        if (!hb || !ha) return false;
        ref = (rb + ra) / 2;  // floor; sub-tick midpoints round down
        break;
    }
    Ticks price = ref + p.offset;
    if (p.cap_limit > 0) {
      price = o.side == Side::Buy ? std::min(price, p.cap_limit)
                                  : std::max(price, p.cap_limit);
    }
    if (price <= 0) {
      return false;
    }
    *out = price;
    return true;
  }

  /**
   * @brief Move a resting pegged order to a new price (loses time priority).
   * @param id        The pegged order id.
   * @param new_price The recomputed price.
   */
  void move_pegged(OrderId id, Ticks new_price) {
    Entry& e = orders.at(id);
    Ladder& L = ladder(e.order.side);
    if (Level* lvl = L.at(e.order.price); lvl != nullptr) {
      lvl->total -= e.order.leaves;
      lvl->displayed -= DisplayedOf(e.order.flags, e.order.leaves, e.slice);
      lvl->fifo.erase(e.pos);
      if (lvl->total == 0) {
        L.on_removed(e.order.price);
      }
    }
    e.order.price = new_price;
    Level& nl = L.touch(new_price);
    nl.fifo.push_back(id);
    e.pos = std::prev(nl.fifo.end());
    nl.total += e.order.leaves;
    nl.displayed += DisplayedOf(e.order.flags, e.order.leaves, e.slice);
    L.on_added(new_price);
  }

  /**
   * @brief Recompute and re-place every pegged order after a BBO change.
   *
   * A single pass suffices: the reference excludes pegged orders, so moving
   * one peg cannot change another's reference.
   */
  void reprice_pegs() {
    if (pegged.empty()) {
      return;
    }
    std::vector<OrderId> ids(pegged.begin(), pegged.end());
    std::sort(ids.begin(), ids.end(), [&](OrderId a, OrderId b) {
      return orders.at(a).order.seq < orders.at(b).order.seq;
    });
    for (const OrderId id : ids) {
      const auto it = orders.find(id);
      if (it == orders.end()) {
        pegged.erase(id);  // filled or cancelled meanwhile
        continue;
      }
      Ticks np = 0;
      if (compute_peg_price(it->second.order, &np) &&
          np != it->second.order.price) {
        move_pegged(id, np);
      }
    }
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
  if (order.type == OrdType::Limit && order.price <= 0 &&
      !order.peg.has_value()) {
    out.accepted = false;
    out.reject_reason = "non-positive limit price";  // pegs price later
    return out;
  }
  if (impl_->orders.find(order.id) != impl_->orders.end() ||
      impl_->stops.find(order.id) != impl_->stops.end()) {
    out.accepted = false;
    out.reject_reason = "duplicate order id";
    return out;
  }

  // Contingent children are held out of the book until their parent fills.
  if (order.link.has_value()) {
    const LinkSpec::Role role = order.link->role;
    const std::uint64_t gid = order.link->group_id;
    if (role == LinkSpec::Role::OtoChild) {
      impl_->groups[gid].pending_child = order;
      out.held = true;
      return out;
    }
    if (role == LinkSpec::Role::BracketTakeProfit ||
        role == LinkSpec::Role::BracketStopLoss) {
      impl_->groups[gid].pending_oco.push_back(order);
      out.held = true;
      return out;
    }
  }

  // Stop / trailing orders: park until the trigger fires (or fire now if the
  // reference price is already past it), rather than matching immediately.
  if (order.trigger.has_value() && !order.trigger->triggered) {
    impl_->arm_stop(order);
    if (impl_->have_last && impl_->stop_triggered(order, impl_->last_price)) {
      order.trigger->triggered = true;
      impl_->inject(order, out.trades);
      out.filled = order.filled;
      out.rested = order.leaves > 0 && order.type != OrdType::Market &&
                   order.tif != Tif::IOC;
      impl_->evaluate_stops(out.trades);
    } else {
      impl_->stops.emplace(order.id, order);
      out.pending_trigger = true;
    }
    return out;
  }

  // Pegged orders rest at a price derived from the (non-pegged) BBO and
  // reprice as it moves. They do not match on entry.
  if (order.peg.has_value()) {
    Ticks price = 0;
    if (!impl_->compute_peg_price(order, &price)) {
      out.accepted = false;
      out.reject_reason = "no reference price for peg";
      return out;
    }
    order.type = OrdType::Limit;
    order.price = price;
    impl_->rest(order);
    impl_->pegged.insert(order.id);
    out.rested = true;
    return out;
  }

  // Post-Only must never take liquidity.
  if (HasFlag(order.flags, OrderFlag::kPostOnly) && impl_->crosses(order)) {
    out.accepted = false;
    out.reject_reason = "post-only would cross";
    return out;
  }

  // All-or-none: never partially fill. If it cannot be fully filled now, an
  // immediate order (IOC/FOK/market) is rejected, while a resting (GTC/DAY)
  // AON order rests as a maker -- the matcher skips it until it can fill in
  // full. When it is fully fillable, fall through and match completely.
  if (HasFlag(order.flags, OrderFlag::kAon)) {
    if (impl_->available_fill(order) < order.leaves) {
      if (order.tif == Tif::IOC || order.type == OrdType::Market) {
        out.accepted = false;
        out.reject_reason = "all-or-none not fully fillable";
        return out;
      }
      impl_->rest(order);
      out.rested = true;
      impl_->reprice_pegs();
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

  // Any trade may have moved the last price into a stop's trigger; cascade.
  if (!out.trades.empty()) {
    impl_->evaluate_stops(out.trades);
  }
  // The book (and thus the reference BBO) may have moved; reprice pegs.
  impl_->reprice_pegs();

  // Register active contingent orders and react to any fills they had.
  if (order.link.has_value()) {
    const LinkSpec::Role role = order.link->role;
    const std::uint64_t gid = order.link->group_id;
    if (role == LinkSpec::Role::OcoLeg) {
      impl_->groups[gid].oco_legs.push_back(order.id);
    } else if (role == LinkSpec::Role::OtoParent ||
               role == LinkSpec::Role::BracketParent) {
      impl_->groups[gid].parent = order.id;
    }
    if (out.rested) {
      impl_->order_group[order.id] = gid;
    }
  }
  process_link_fills(order, out);
  return out;
}

void OrderBook::process_link_fills(const Order& submitted, SubmitOutcome& out) {
  // Which linked orders filled in this submit? The aggressor itself, plus any
  // resting linked makers that were hit.
  std::vector<OrderId> filled;
  if (submitted.link.has_value() && submitted.filled > 0) {
    filled.push_back(submitted.id);
  }
  for (const Trade& t : out.trades) {
    if (impl_->order_group.count(t.maker_id) > 0) {
      filled.push_back(t.maker_id);
    }
  }
  std::sort(filled.begin(), filled.end());
  filled.erase(std::unique(filled.begin(), filled.end()), filled.end());

  for (const OrderId id : filled) {
    std::uint64_t gid = 0;
    if (submitted.link.has_value() && id == submitted.id) {
      gid = submitted.link->group_id;
    } else if (const auto it = impl_->order_group.find(id);
               it != impl_->order_group.end()) {
      gid = it->second;
    } else {
      continue;
    }
    const auto git = impl_->groups.find(gid);
    if (git == impl_->groups.end()) {
      continue;
    }

    // OCO: a filled leg cancels its siblings.
    std::vector<OrderId>& legs = git->second.oco_legs;
    if (std::find(legs.begin(), legs.end(), id) != legs.end()) {
      for (const OrderId other : legs) {
        if (other != id) {
          impl_->order_group.erase(other);
          cancel(other);
        }
      }
      legs.clear();
      impl_->order_group.erase(id);
    }

    // Parent fully filled: release the group's children.
    const bool fully = (submitted.link.has_value() && id == submitted.id)
                           ? submitted.leaves == 0
                           : impl_->orders.find(id) == impl_->orders.end();
    if (impl_->groups[gid].parent == id && fully) {
      release_children(gid, out);
    }
  }
}

void OrderBook::release_children(std::uint64_t gid, SubmitOutcome& out) {
  // OTO: release the single pending child as an ordinary active order.
  std::optional<Order> child;
  child.swap(impl_->groups[gid].pending_child);
  if (child.has_value()) {
    Order c = *child;
    c.link.reset();
    const SubmitOutcome co = submit(c);
    out.trades.insert(out.trades.end(), co.trades.begin(), co.trades.end());
  }

  // Bracket: release take-profit and stop-loss as a mutually-cancelling OCO.
  std::vector<Order> pend;
  pend.swap(impl_->groups[gid].pending_oco);
  for (Order c : pend) {
    LinkSpec link;
    link.role = LinkSpec::Role::OcoLeg;
    link.group_id = gid;
    c.link = link;
    const SubmitOutcome co = submit(c);
    out.trades.insert(out.trades.end(), co.trades.begin(), co.trades.end());
  }
}

bool OrderBook::cancel(OrderId id) {
  // A parked stop order may be cancelled before it triggers.
  if (const auto s = impl_->stops.find(id); s != impl_->stops.end()) {
    impl_->stops.erase(s);
    return true;
  }
  const auto it = impl_->orders.find(id);
  if (it == impl_->orders.end()) {
    return false;
  }
  const bool was_pegged = impl_->pegged.erase(id) > 0;
  Impl::Entry& e = it->second;
  const Side side = e.order.side;
  const Ticks price = e.order.price;
  const Quantity leaves = e.order.leaves;
  const Quantity displayed = DisplayedOf(e.order.flags, leaves, e.slice);
  Ladder& own = impl_->ladder(side);
  if (Level* lvl = own.at(price); lvl != nullptr) {
    lvl->total -= leaves;
    lvl->displayed -= displayed;
    lvl->fifo.erase(e.pos);
    if (lvl->total == 0) {
      own.on_removed(price);
    }
  }
  impl_->orders.erase(it);
  // Cancelling a non-pegged order can move the reference BBO.
  if (!was_pegged) {
    impl_->reprice_pegs();
  }
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

Quantity OrderBook::displayed_qty_at(Side side, Ticks price) const {
  Ladder& l = impl_->ladder(side);
  Level* lvl = l.at(price);
  return lvl == nullptr ? 0 : lvl->displayed;
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

bool OrderBook::last_trade_price(Ticks* out) const {
  if (!impl_->have_last) {
    return false;
  }
  *out = impl_->last_price;
  return true;
}

std::size_t OrderBook::resting_count() const { return impl_->orders.size(); }

std::size_t OrderBook::pending_stop_count() const {
  return impl_->stops.size();
}

}  // namespace codicis
