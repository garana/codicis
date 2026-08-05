/**
 * @file order_book.cc
 * @brief Implementation of the dense-ladder order book and matcher.
 */

#include "codicis/core/order_book.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory_resource>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codicis {
namespace {

/**
 * @brief One price level: FIFO of resting order ids plus running totals.
 *
 * Allocator-aware (uses-allocator protocol) so that when it lives inside a
 * std::pmr::deque the level's FIFO list draws its nodes from the same pooled
 * memory resource as the rest of the book.
 */
struct Level {
  using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

  Quantity total = 0;      /**< Matchable quantity (incl. hidden/reserve). */
  Quantity displayed = 0;  /**< Publicly visible quantity. */
  std::pmr::list<OrderId> fifo;

  explicit Level(const allocator_type& alloc = {}) : fifo(alloc) {}
  Level(const Level& o, const allocator_type& alloc = {})
      : total(o.total), displayed(o.displayed), fifo(o.fifo, alloc) {}
  Level(Level&& o, const allocator_type& alloc)
      : total(o.total), displayed(o.displayed), fifo(std::move(o.fifo), alloc) {}
  Level(Level&&) = default;
  Level& operator=(const Level&) = default;
  Level& operator=(Level&&) = default;
  ~Level() = default;
};

/** @brief True if @p flags select the opening auction (MOO/LOO/OPG). */
bool IsOpeningAuction(std::uint32_t flags) {
  return HasFlag(flags, OrderFlag::kMoo) || HasFlag(flags, OrderFlag::kLoo) ||
         HasFlag(flags, OrderFlag::kOpg);
}

/** @brief True if @p flags select the closing auction (MOC/LOC). */
bool IsClosingAuction(std::uint32_t flags) {
  return HasFlag(flags, OrderFlag::kMoc) || HasFlag(flags, OrderFlag::kLoc);
}

/** @brief All auction flag bits, cleared from a remainder entering the LOB. */
constexpr std::uint32_t kAuctionMask =
    static_cast<std::uint32_t>(OrderFlag::kMoo) |
    static_cast<std::uint32_t>(OrderFlag::kLoo) |
    static_cast<std::uint32_t>(OrderFlag::kMoc) |
    static_cast<std::uint32_t>(OrderFlag::kLoc) |
    static_cast<std::uint32_t>(OrderFlag::kOpg);

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
  Ladder(Side s, std::pmr::memory_resource* mr) : side(s), levels(mr) {}

  Side side;
  std::pmr::deque<Level> levels;
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
    std::pmr::list<OrderId>::iterator pos;
    Quantity slice = 0;  /**< Iceberg visible slice remaining (else unused). */
  };

  /** @brief A contingent-order group (OCO / OTO / bracket). */
  struct Group {
    OrderId parent = 0;               /**< OTO/bracket parent order id. */
    std::vector<OrderId> oco_legs;    /**< Active mutually-cancelling legs. */
    std::optional<Order> pending_child;  /**< OTO child, released on fill. */
    std::vector<Order> pending_oco;   /**< Bracket TP/SL, released as OCO. */
  };

  /** @brief Per-account risk state, keyed by client id, for reduce-only. */
  struct Position {
    Quantity net = 0;            /**< Signed net position (+long, -short). */
    Quantity reserved_sell = 0;  /**< Resting reduce-only SELL leaves. */
    Quantity reserved_buy = 0;   /**< Resting reduce-only BUY leaves. */
  };

  // One pooled memory resource backs every node-allocating container below, so
  // steady-state submit/cancel recycles freed nodes instead of calling the
  // global allocator each time. The core is single-threaded, so the
  // unsynchronized (lock-free) pool is safe. Declared first, hence destroyed
  // last -- after every container that draws from it.
  std::pmr::unsynchronized_pool_resource pool_;
  Ladder bids;
  Ladder asks;
  std::pmr::unordered_map<OrderId, Entry> orders;
  std::pmr::unordered_map<OrderId, Order> stops;    // parked, awaiting trigger
  std::pmr::unordered_set<OrderId> pegged;          // resting orders that reprice
  std::pmr::unordered_map<std::uint64_t, Group> groups;      // contingent groups
  std::pmr::unordered_map<OrderId, std::uint64_t> order_group;  // linked->group
  std::pmr::unordered_map<ClientId, Position> positions;  // reduce-only risk
  std::vector<Order> opening_auction;  // MOO/LOO/OPG awaiting the open cross
  std::vector<Order> closing_auction;  // MOC/LOC awaiting the close cross

  /** @brief Deep (non-resident) liquidity summary for one side. */
  struct Deep {
    bool has = false;      /**< Any liquidity beyond the resident window. */
    Ticks boundary = 0;    /**< Nearest deep price (adjacent to the window). */
  };
  std::size_t mem_levels = 0;  // max resident populated levels/side (0=unbounded)
  Deep deep_bid;
  Deep deep_ask;
  SeqNo next_seq = 1;
  StpPolicy stp = StpPolicy::kNone;
  bool have_last = false;
  Ticks last_price = 0;

  Symbol symbol_;                       // tags emitted book events
  BookEventSink* event_sink_ = nullptr; // nullptr disables emission

  /**
   * @brief Emit an Add/Cancel/Replenish/Trigger event for order @p o.
   * @param qty       The matchable quantity (leaves, or the new slice).
   * @param displayed The lit portion (0 hidden, slice iceberg, else == qty).
   */
  void emit_event(BookEventType type, const Order& o, Quantity qty,
                  Quantity displayed) {
    if (event_sink_ == nullptr) {
      return;
    }
    BookEvent ev;
    ev.symbol = symbol_;
    ev.type = type;
    ev.order_id = o.id;
    ev.side = o.side;
    ev.price = o.price;
    ev.qty = qty;
    ev.displayed = displayed;
    event_sink_->on_book_event(ev);
  }

  /**
   * @brief Emit a Trade event for execution @p t.
   * @param displayed The lit quantity the fill consumed (0 if the maker is
   *                  hidden, else the fill).
   */
  void emit_trade(const Trade& t, Quantity displayed) {
    if (event_sink_ == nullptr) {
      return;
    }
    BookEvent ev;
    ev.symbol = symbol_;
    ev.type = BookEventType::kTrade;
    ev.order_id = t.maker_id;
    ev.taker_id = t.taker_id;
    ev.side = t.taker_side;
    ev.price = t.price;
    ev.qty = t.qty;
    ev.displayed = displayed;
    event_sink_->on_book_event(ev);
  }

  /** @brief Emit a Reprice event for a pegged order moving old -> new price. */
  void emit_reprice(const Order& o, Ticks old_price, Ticks new_price,
                    Quantity qty, Quantity displayed) {
    if (event_sink_ == nullptr) {
      return;
    }
    BookEvent ev;
    ev.symbol = symbol_;
    ev.type = BookEventType::kReprice;
    ev.order_id = o.id;
    ev.side = o.side;
    ev.prev_price = old_price;
    ev.price = new_price;
    ev.qty = qty;
    ev.displayed = displayed;
    event_sink_->on_book_event(ev);
  }

  /** @brief Wire every pooled container to the shared memory resource. */
  Impl()
      : bids(Side::Buy, &pool_),
        asks(Side::Sell, &pool_),
        orders(&pool_),
        stops(&pool_),
        pegged(&pool_),
        groups(&pool_),
        order_group(&pool_),
        positions(&pool_) {}

  /** @return The ladder for side s. */
  Ladder& ladder(Side s) { return s == Side::Buy ? bids : asks; }

  /** @return True if o is a reduce-only order. */
  static bool is_reduce_only(const Order& o) {
    return HasFlag(o.flags, OrderFlag::kReduceOnly);
  }

  /** @brief Apply a fill of @p q to a client's net position (skip anonymous). */
  void apply_position(ClientId c, Side s, Quantity q) {
    if (c == 0) {
      return;
    }
    positions[c].net += (s == Side::Buy ? q : -q);
  }

  /** @return The reserved reduce-only leaves resting on side @p s. */
  static Quantity& reserved_on(Position& p, Side s) {
    return s == Side::Sell ? p.reserved_sell : p.reserved_buy;
  }

  /** @brief Release @p q of a resting reduce-only order's reservation. */
  void release_reserved(const Order& o, Quantity q) {
    if (!is_reduce_only(o) || o.client_id == 0) {
      return;
    }
    const auto it = positions.find(o.client_id);
    if (it != positions.end()) {
      reserved_on(it->second, o.side) -= q;
    }
  }

  /**
   * @brief Quantity a reduce-only order may still execute for its account.
   *
   * A sell may only shrink a long, a buy only a short; each is bounded by the
   * net position minus the reduce-only leaves already resting on that side, so
   * concurrent reduce-only orders cannot together flip the position.
   * @param o The reduce-only order.
   * @return The reducible quantity (<= 0 means nothing to reduce).
   */
  Quantity reduce_only_capacity(const Order& o) {
    const auto it = positions.find(o.client_id);
    if (it == positions.end()) {
      return 0;
    }
    const Position& p = it->second;
    return o.side == Side::Sell ? p.net - p.reserved_sell
                                : -p.net - p.reserved_buy;
  }

  /**
   * @brief The most aggressive price @p o will take.
   *
   * A discretionary order displays at its limit price but is willing to reach
   * into the spread by its discretion band when it is the aggressor: a buy will
   * pay up to price+discretion, a sell will accept down to price-discretion.
   * (The band is exercised on entry; the resting remainder shows at the plain
   * limit -- a documented simplification, like pegs that do not auto-match.)
   * @param o The order.
   * @return o.price widened by the discretion band, or o.price if none.
   */
  Ticks effective_limit(const Order& o) const {
    if (!HasFlag(o.flags, OrderFlag::kDiscretion) || o.discretion <= 0) {
      return o.price;
    }
    return o.side == Side::Buy ? o.price + o.discretion
                               : o.price - o.discretion;
  }

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
    const Ticks lim = effective_limit(o);
    return o.side == Side::Buy ? lim >= bp : lim <= bp;
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
    const Ticks lim = effective_limit(o);
    auto acceptable = [&](Ticks p) -> bool {
      if (o.type == OrdType::Market) {
        return true;
      }
      return o.side == Side::Buy ? lim >= p : lim <= p;
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
          release_reserved(me.order, me.order.leaves);
          // The resting maker leaves the book without a fill (STP cancel).
          emit_event(BookEventType::kCancel, me.order, me.order.leaves,
                     DisplayedOf(me.order.flags, me.order.leaves, me.slice));
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
      // A hidden maker shows nothing, so its fills consume 0 lit quantity;
      // otherwise the fill reduces the displayed depth by the same amount.
      emit_trade(trades.back(), hidden ? 0 : fill);
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
      // Update both accounts' net positions, and release the maker's reduce-
      // only reservation as it fills (the aggressor's is released when/if its
      // remainder rests).
      apply_position(o.client_id, o.side, fill);
      apply_position(me.order.client_id, me.order.side, fill);
      release_reserved(me.order, fill);

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
        // The refreshed slice becomes lit again (total is unchanged).
        emit_event(BookEventType::kReplenish, me.order, me.slice, me.slice);
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
    const Ticks lim = effective_limit(o);
    auto acceptable = [&](Ticks p) {
      if (o.type == OrdType::Market) {
        return true;
      }
      return o.side == Side::Buy ? lim >= p : lim <= p;
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

  /** @return The deep-liquidity summary for side @p s. */
  Deep& deep_of(Side s) { return s == Side::Buy ? deep_bid : deep_ask; }
  const Deep& deep_of(Side s) const {
    return s == Side::Buy ? deep_bid : deep_ask;
  }

  /** @return The number of populated (total > 0) levels on ladder @p L. */
  std::size_t populated_count(const Ladder& L) const {
    std::size_t n = 0;
    for (const Level& lvl : L.levels) {
      if (lvl.total > 0) {
        ++n;
      }
    }
    return n;
  }

  /**
   * @brief The worst (farthest-from-top) resident price on ladder @p L.
   * @param L   The ladder.
   * @param out Receives the price on success.
   * @return True if the ladder has any populated level.
   */
  bool worst_price(const Ladder& L, Ticks* out) const {
    if (!L.has_base) {
      return false;
    }
    if (L.side == Side::Buy) {  // worst bid = lowest populated price
      for (std::size_t i = 0; i < L.levels.size(); ++i) {
        if (L.levels[i].total > 0) {
          *out = L.base + static_cast<Ticks>(i);
          return true;
        }
      }
    } else {  // worst ask = highest populated price
      for (std::size_t i = L.levels.size(); i-- > 0;) {
        if (L.levels[i].total > 0) {
          *out = L.base + static_cast<Ticks>(i);
          return true;
        }
      }
    }
    return false;
  }

  /** @return True if price @p p is worse (farther from top) than @p ref. */
  static bool worse_than(Side s, Ticks p, Ticks ref) {
    return s == Side::Buy ? p < ref : p > ref;
  }

  /** @brief Record that @p price now holds deep liquidity on side @p s. */
  void note_deep(Side s, Ticks price) {
    Deep& d = deep_of(s);
    // The nearest deep price is the one closest to the resident window: the
    // highest deep bid, or the lowest deep ask.
    if (!d.has) {
      d.boundary = price;
    } else {
      d.boundary = s == Side::Buy ? std::max(d.boundary, price)
                                  : std::min(d.boundary, price);
    }
    d.has = true;
  }

  /** @brief Evict the worst resident level to deep, collecting its orders. */
  void evict_worst(Ladder& L, std::vector<Order>* evicted) {
    Ticks wp = 0;
    if (!worst_price(L, &wp)) {
      return;
    }
    Level* lvl = L.at(wp);
    if (lvl == nullptr) {
      return;
    }
    for (const OrderId id : lvl->fifo) {
      const auto it = orders.find(id);
      if (it != orders.end()) {
        if (evicted != nullptr) {
          evicted->push_back(it->second.order);
        }
        pegged.erase(id);  // a deep order does not reprice in memory
        orders.erase(it);  // note: reduce-only reservation is retained
      }
    }
    lvl->fifo.clear();
    lvl->total = 0;
    lvl->displayed = 0;
    L.on_removed(wp);
    note_deep(L.side, wp);  // this level is now the nearest deep one
  }

  /**
   * @brief Rest the remainder of o on its own side, honoring the window.
   *
   * With a bounded window (mem_levels > 0): an order that would open a NEW
   * price level worse than the current worst resident level, when the side is
   * already full, is NOT materialized -- it rests deep (persisted elsewhere).
   * A better order that overgrows the window evicts the worst resident level.
   * @param o       The order with leaves > 0.
   * @param evicted If non-null, receives any levels pushed out to deep.
   * @return True if the order was materialized resident; false if it rests deep.
   */
  bool rest(const Order& o, std::vector<Order>* evicted = nullptr) {
    Ladder& own = ladder(o.side);
    bool new_level = true;
    if (mem_levels > 0) {
      const Level* existing = own.at(o.price);
      new_level = existing == nullptr || existing->total == 0;
      if (new_level) {
        Ticks worst = 0;
        if (worst_price(own, &worst) && populated_count(own) >= mem_levels &&
            worse_than(o.side, o.price, worst)) {
          note_deep(o.side, o.price);  // beyond the window -> rests deep
          const Quantity dslice = HasFlag(o.flags, OrderFlag::kIceberg)
                                      ? std::min(o.display_qty, o.leaves)
                                      : 0;
          emit_event(BookEventType::kAdd, o, o.leaves,  // joins the book (deep)
                     DisplayedOf(o.flags, o.leaves, dslice));
          return false;
        }
      }
    }

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
    // A resting reduce-only order commits to reducing the position when it
    // fills; reserve its leaves so a later reduce-only order cannot double-book
    // the same position and flip it.
    if (is_reduce_only(o) && o.client_id != 0) {
      reserved_on(positions[o.client_id], o.side) += o.leaves;
    }
    // A new level better than the window's far edge overgrows it: evict.
    if (mem_levels > 0 && new_level && populated_count(own) > mem_levels) {
      evict_worst(own, evicted);
    }
    emit_event(BookEventType::kAdd, o, o.leaves,  // joins the book (resident)
               DisplayedOf(o.flags, o.leaves, slice));
    return true;
  }

  /**
   * @brief Materialize a pulled-back resting order without matching/windowing.
   *
   * The order is already known-resting (from storage); place it directly. Its
   * reduce-only reservation was retained across eviction, so it is not
   * re-applied here.
   * @param o The resting order to insert.
   */
  void insert_resident(const Order& o) {
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
        // Lifecycle marker: the stop fired. Its resulting depth/executions
        // follow as the inject's own Add/Trade events.
        // A trigger is a lifecycle marker, not depth; displayed is irrelevant.
        emit_event(BookEventType::kTrigger, o, o.leaves, 0);
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
    const Ticks old_price = e.order.price;
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
    emit_reprice(e.order, old_price, new_price, e.order.leaves,
                 DisplayedOf(e.order.flags, e.order.leaves, e.slice));
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

  /**
   * @brief Cross a queue of auction orders at a single uniform clearing price.
   *
   * The clearing price maximizes executed volume; ties are broken by smallest
   * order imbalance, then by proximity to the last trade price, then by the
   * lower price. All eligible orders trade at that one price with market-first,
   * then price, then arrival-sequence priority. Unfilled LOO/LOC limit
   * remainders enter continuous trading (match + rest); MOO/MOC/OPG remainders
   * are discarded. The queue is emptied.
   * @param queue The auction order queue (consumed).
   * @return The auction executions, plus any continuous cascade they trigger.
   */
  std::vector<Trade> cross_auction(std::vector<Order>& queue) {
    std::vector<Trade> trades;
    if (queue.empty()) {
      return trades;
    }

    std::vector<Order*> buys;
    std::vector<Order*> sells;
    for (Order& o : queue) {
      (o.side == Side::Buy ? buys : sells).push_back(&o);
    }
    auto is_market = [](const Order* o) { return o->type == OrdType::Market; };
    auto demand_at = [&](Ticks p) {
      Quantity d = 0;
      for (const Order* o : buys) {
        if (is_market(o) || o->price >= p) {
          d += o->leaves;
        }
      }
      return d;
    };
    auto supply_at = [&](Ticks p) {
      Quantity s = 0;
      for (const Order* o : sells) {
        if (is_market(o) || o->price <= p) {
          s += o->leaves;
        }
      }
      return s;
    };

    // Candidate clearing prices are the distinct limit prices; with none (all
    // market) fall back to the last trade price as the reference.
    std::set<Ticks> candidates;
    for (const Order* o : buys) {
      if (!is_market(o)) {
        candidates.insert(o->price);
      }
    }
    for (const Order* o : sells) {
      if (!is_market(o)) {
        candidates.insert(o->price);
      }
    }

    bool have_p = false;
    Ticks pstar = 0;
    Quantity best_v = 0;
    Quantity best_imb = 0;
    auto consider = [&](Ticks p) {
      const Quantity d = demand_at(p);
      const Quantity s = supply_at(p);
      const Quantity v = std::min(d, s);
      if (v <= 0) {
        return;
      }
      const Quantity imb = d > s ? d - s : s - d;
      bool better = !have_p || v > best_v || (v == best_v && imb < best_imb);
      if (!better && v == best_v && imb == best_imb) {
        if (have_last) {
          const Ticks da = p > last_price ? p - last_price : last_price - p;
          const Ticks db =
              pstar > last_price ? pstar - last_price : last_price - pstar;
          better = da < db;
        } else {
          better = p < pstar;
        }
      }
      if (better) {
        have_p = true;
        pstar = p;
        best_v = v;
        best_imb = imb;
      }
    };
    if (candidates.empty()) {
      if (have_last) {
        consider(last_price);
      }
    } else {
      for (const Ticks p : candidates) {
        consider(p);
      }
    }

    if (have_p) {
      // Market orders first, then buys by higher price / sells by lower price,
      // ties by arrival sequence.
      std::sort(buys.begin(), buys.end(), [&](const Order* a, const Order* b) {
        if (is_market(a) != is_market(b)) return is_market(a);
        if (a->price != b->price) return a->price > b->price;
        return a->seq < b->seq;
      });
      std::sort(sells.begin(), sells.end(), [&](const Order* a, const Order* b) {
        if (is_market(a) != is_market(b)) return is_market(a);
        if (a->price != b->price) return a->price < b->price;
        return a->seq < b->seq;
      });
      auto buy_ok = [&](const Order* o) {
        return is_market(o) || o->price >= pstar;
      };
      auto sell_ok = [&](const Order* o) {
        return is_market(o) || o->price <= pstar;
      };

      Quantity remaining = best_v;
      std::size_t bi = 0;
      std::size_t si = 0;
      while (remaining > 0 && bi < buys.size() && si < sells.size()) {
        if (!buy_ok(buys[bi]) || !sell_ok(sells[si])) {
          break;  // sorted: once ineligible, no eligible orders remain
        }
        Order* b = buys[bi];
        Order* s = sells[si];
        const Quantity q = std::min({b->leaves, s->leaves, remaining});
        trades.push_back(Trade{.taker_id = b->id,
                               .maker_id = s->id,
                               .taker_side = Side::Buy,
                               .price = pstar,
                               .qty = q});
        emit_trade(trades.back(),
                   HasFlag(s->flags, OrderFlag::kHidden) ? 0 : q);
        b->leaves -= q;
        b->filled += q;
        s->leaves -= q;
        s->filled += q;
        apply_position(b->client_id, Side::Buy, q);
        apply_position(s->client_id, Side::Sell, q);
        remaining -= q;
        if (b->leaves == 0) {
          ++bi;
        }
        if (s->leaves == 0) {
          ++si;
        }
      }
      last_price = pstar;
      have_last = true;
    }

    // Unfilled LOO/LOC limit remainders enter continuous trading; MOO/MOC/OPG
    // remainders are discarded.
    for (Order& o : queue) {
      if (o.leaves > 0 && (HasFlag(o.flags, OrderFlag::kLoo) ||
                           HasFlag(o.flags, OrderFlag::kLoc))) {
        Order r = o;
        r.flags &= ~kAuctionMask;
        inject(r, trades);  // match against the LOB, then rest the remainder
      }
    }
    queue.clear();

    if (!trades.empty()) {
      evaluate_stops(trades);
      reprice_pegs();
    }
    return trades;
  }
};

OrderBook::OrderBook() : impl_(std::make_unique<Impl>()) {}

OrderBook::OrderBook(StpPolicy stp) : impl_(std::make_unique<Impl>()) {
  impl_->stp = stp;
}

OrderBook::OrderBook(StpPolicy stp, std::size_t mem_levels)
    : impl_(std::make_unique<Impl>()) {
  impl_->stp = stp;
  impl_->mem_levels = mem_levels;
}

OrderBook::~OrderBook() = default;

void OrderBook::set_symbol(Symbol symbol) {
  impl_->symbol_ = std::move(symbol);
}

void OrderBook::set_event_sink(BookEventSink* sink) {
  impl_->event_sink_ = sink;
}

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

  // Auction orders do not trade continuously: queue them until the opening or
  // closing uniform-price cross is run at the session boundary.
  if (IsOpeningAuction(order.flags) || IsClosingAuction(order.flags)) {
    (IsOpeningAuction(order.flags) ? impl_->opening_auction
                                   : impl_->closing_auction)
        .push_back(order);
    out.held = true;
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
    const bool resident = impl_->rest(order, &out.evicted);
    if (resident) {
      impl_->pegged.insert(order.id);  // only resident pegs reprice
    }
    out.rested = resident;
    out.rested_deep = !resident;
    out.resting = order;
    return out;
  }

  // Post-Only must never take liquidity.
  if (HasFlag(order.flags, OrderFlag::kPostOnly) && impl_->crosses(order)) {
    out.accepted = false;
    out.reject_reason = "post-only would cross";
    return out;
  }

  // Reduce-Only: may only shrink an existing position, never open or grow one.
  // Requires an account (client id) to attribute the position to, and is capped
  // at the reducible quantity (net position minus reduce-only leaves already
  // resting on this side). A wrong-side or zero position is rejected outright.
  if (HasFlag(order.flags, OrderFlag::kReduceOnly)) {
    if (order.client_id == 0) {
      out.accepted = false;
      out.reject_reason = "reduce-only requires an account";
      return out;
    }
    const Quantity cap = impl_->reduce_only_capacity(order);
    if (cap <= 0) {
      out.accepted = false;
      out.reject_reason = "reduce-only: no position to reduce";
      return out;
    }
    if (order.leaves > cap) {
      order.leaves = cap;  // cap the order to the reducible size
      order.qty = cap;
    }
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
      const bool resident = impl_->rest(order, &out.evicted);
      out.rested = resident;
      out.rested_deep = !resident;
      out.resting = order;
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
      const bool resident = impl_->rest(order, &out.evicted);
      out.rested = resident;
      out.rested_deep = !resident;
      out.resting = order;
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
  // An order queued for an auction may be cancelled before the cross runs.
  for (std::vector<Order>* q : {&impl_->opening_auction,
                                &impl_->closing_auction}) {
    const auto qit = std::find_if(q->begin(), q->end(),
                                  [id](const Order& o) { return o.id == id; });
    if (qit != q->end()) {
      q->erase(qit);
      return true;
    }
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
  // The order leaves the book other than by a fill (explicit cancel, GTD/DAY
  // expiry via expire(), or an OCO sibling cancel routed through here).
  impl_->emit_event(BookEventType::kCancel, e.order, leaves, displayed);
  impl_->release_reserved(e.order, leaves);  // a resting reduce-only order
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

void OrderBook::seed_position(ClientId client, Quantity net) {
  if (client == 0) {
    return;
  }
  impl_->positions[client].net = net;
}

Quantity OrderBook::position(ClientId client) const {
  const auto it = impl_->positions.find(client);
  return it == impl_->positions.end() ? 0 : it->second.net;
}

void OrderBook::insert_resident(const Order& order) {
  impl_->insert_resident(order);
}

bool OrderBook::has_deep(Side side) const {
  return impl_->deep_of(side).has;
}

bool OrderBook::deep_boundary(Side side, Ticks* out) const {
  const Impl::Deep& d = impl_->deep_of(side);
  if (!d.has) {
    return false;
  }
  *out = d.boundary;
  return true;
}

bool OrderBook::worst_resident(Side side, Ticks* out) const {
  return impl_->worst_price(impl_->ladder(side), out);
}

std::vector<Trade> OrderBook::run_opening_auction() {
  return impl_->cross_auction(impl_->opening_auction);
}

std::vector<Trade> OrderBook::run_closing_auction() {
  return impl_->cross_auction(impl_->closing_auction);
}

std::size_t OrderBook::auction_count(bool opening) const {
  return opening ? impl_->opening_auction.size()
                 : impl_->closing_auction.size();
}

std::size_t OrderBook::resting_count() const { return impl_->orders.size(); }

std::size_t OrderBook::pending_stop_count() const {
  return impl_->stops.size();
}

}  // namespace codicis
