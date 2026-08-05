/**
 * @file book_replica.cc
 * @brief Implementation of BookReplica (see book_replica.h).
 */

#include "codicis/feed/book_replica.h"

namespace codicis {

void BookReplica::bump(std::map<Ticks, Quantity>& m, Ticks price,
                       Quantity delta) {
  const auto it = m.find(price);
  if (it == m.end()) {
    if (delta > 0) {
      m.emplace(price, delta);
    }
    return;
  }
  it->second += delta;
  if (it->second <= 0) {
    m.erase(it);
  }
}

bool BookReplica::apply(const BookEvent& ev) {
  // Gap detection: the upstream feed is best-effort, so a jump in seq means an
  // event was lost and the replica may be stale until a resync.
  bool gap = false;
  if (last_seq_ != 0 && ev.seq != 0 && ev.seq != last_seq_ + 1) {
    gap = true;
    ++gaps_;
  }
  if (ev.seq > last_seq_) {
    last_seq_ = ev.seq;
  }

  SymBook& b = books_[ev.symbol];
  switch (ev.type) {
    case BookEventType::kAdd: {
      b.orders[ev.order_id] =
          OrderView{ev.order_id, ev.side, ev.price, ev.qty, ev.displayed};
      bump(side_map(b, ev.side), ev.price, ev.qty);
      bump(disp_map(b, ev.side), ev.price, ev.displayed);
      break;
    }
    case BookEventType::kCancel: {
      const auto it = b.orders.find(ev.order_id);
      if (it != b.orders.end()) {
        bump(side_map(b, it->second.side), it->second.price, -it->second.qty);
        bump(disp_map(b, it->second.side), it->second.price,
             -it->second.displayed);
        b.orders.erase(it);
      }
      break;
    }
    case BookEventType::kTrade: {
      // The resting maker (order_id) shrinks by the fill; the taker is the
      // aggressor and never rests here (its remainder arrives as its own Add).
      // ev.qty is the matchable fill; ev.displayed is the lit portion consumed
      // (0 for a hidden maker).
      const auto it = b.orders.find(ev.order_id);
      if (it != b.orders.end()) {
        it->second.qty -= ev.qty;
        it->second.displayed -= ev.displayed;
        bump(side_map(b, it->second.side), it->second.price, -ev.qty);
        bump(disp_map(b, it->second.side), it->second.price, -ev.displayed);
        if (it->second.qty <= 0) {
          b.orders.erase(it);
        }
      }
      break;
    }
    case BookEventType::kReprice: {
      // A pegged order moved prev_price -> price (leaves unchanged).
      const auto it = b.orders.find(ev.order_id);
      if (it != b.orders.end()) {
        OrderView& ov = it->second;
        bump(side_map(b, ov.side), ov.price, -ov.qty);
        bump(disp_map(b, ov.side), ov.price, -ov.displayed);
        ov.price = ev.price;
        ov.displayed = ev.displayed;
        bump(side_map(b, ov.side), ov.price, ov.qty);
        bump(disp_map(b, ov.side), ov.price, ov.displayed);
      }
      break;
    }
    case BookEventType::kReplenish: {
      // Matchable depth is unchanged (the reserve was counted at Add and the
      // fills came as Trades); the refreshed slice becomes lit again.
      const auto it = b.orders.find(ev.order_id);
      if (it != b.orders.end()) {
        it->second.displayed += ev.displayed;
        bump(disp_map(b, it->second.side), it->second.price, ev.displayed);
      }
      break;
    }
    case BookEventType::kTrigger:
      // Lifecycle marker only: the injected order's depth/executions arrive as
      // their own Add/Trade events.
      break;
  }
  return gap;
}

bool BookReplica::best_of(const Symbol& symbol, Side side, bool displayed,
                          Ticks* price, Quantity* qty) const {
  const auto bit = books_.find(symbol);
  if (bit == books_.end()) {
    return false;
  }
  const SymBook& b = bit->second;
  const std::map<Ticks, Quantity>& m =
      displayed ? (side == Side::Buy ? b.disp_bids : b.disp_asks)
                : (side == Side::Buy ? b.bids : b.asks);
  if (m.empty()) {
    return false;
  }
  // Highest bid / lowest ask.
  const auto& best = side == Side::Buy ? *m.rbegin() : *m.begin();
  *price = best.first;
  *qty = best.second;
  return true;
}

std::vector<BookReplica::Level> BookReplica::depth_of(const Symbol& symbol,
                                                      Side side, bool displayed,
                                                      std::size_t max) const {
  std::vector<Level> out;
  const auto bit = books_.find(symbol);
  if (bit == books_.end()) {
    return out;
  }
  const SymBook& b = bit->second;
  const std::map<Ticks, Quantity>& m =
      displayed ? (side == Side::Buy ? b.disp_bids : b.disp_asks)
                : (side == Side::Buy ? b.bids : b.asks);
  if (side == Side::Buy) {
    for (auto it = m.rbegin(); it != m.rend(); ++it) {  // highest first
      if (max != 0 && out.size() >= max) {
        break;
      }
      out.push_back(Level{it->first, it->second});
    }
  } else {
    for (auto it = m.begin(); it != m.end(); ++it) {  // lowest first
      if (max != 0 && out.size() >= max) {
        break;
      }
      out.push_back(Level{it->first, it->second});
    }
  }
  return out;
}

bool BookReplica::best_bid(const Symbol& symbol, Ticks* price,
                           Quantity* qty) const {
  return best_of(symbol, Side::Buy, /*displayed=*/false, price, qty);
}

bool BookReplica::best_ask(const Symbol& symbol, Ticks* price,
                           Quantity* qty) const {
  return best_of(symbol, Side::Sell, /*displayed=*/false, price, qty);
}

bool BookReplica::best_displayed_bid(const Symbol& symbol, Ticks* price,
                                     Quantity* qty) const {
  return best_of(symbol, Side::Buy, /*displayed=*/true, price, qty);
}

bool BookReplica::best_displayed_ask(const Symbol& symbol, Ticks* price,
                                     Quantity* qty) const {
  return best_of(symbol, Side::Sell, /*displayed=*/true, price, qty);
}

std::vector<BookReplica::Level> BookReplica::depth(const Symbol& symbol,
                                                   Side side,
                                                   std::size_t max) const {
  return depth_of(symbol, side, /*displayed=*/false, max);
}

std::vector<BookReplica::Level> BookReplica::displayed_depth(
    const Symbol& symbol, Side side, std::size_t max) const {
  return depth_of(symbol, side, /*displayed=*/true, max);
}

std::vector<BookReplica::OrderView> BookReplica::orders_at(const Symbol& symbol,
                                                           Side side,
                                                           Ticks price) const {
  std::vector<OrderView> out;
  const auto bit = books_.find(symbol);
  if (bit == books_.end()) {
    return out;
  }
  for (const auto& [id, ov] : bit->second.orders) {
    if (ov.side == side && ov.price == price) {
      out.push_back(ov);
    }
  }
  return out;
}

}  // namespace codicis
