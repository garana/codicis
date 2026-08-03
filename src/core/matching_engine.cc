/**
 * @file matching_engine.cc
 * @brief Implementation of the per-symbol MatchingEngine (see the header).
 */

#include "codicis/core/matching_engine.h"

#include <utility>

namespace codicis {

OrderBook& MatchingEngine::book_for(const Symbol& symbol) {
  const auto it = books_.find(symbol);
  if (it != books_.end()) {
    return *it->second;
  }
  auto book = std::make_unique<OrderBook>(stp_);
  OrderBook& ref = *book;
  books_.emplace(symbol, std::move(book));
  return ref;
}

const OrderBook* MatchingEngine::book_of(const Symbol& symbol) const {
  const auto it = books_.find(symbol);
  return it == books_.end() ? nullptr : it->second.get();
}

SubmitOutcome MatchingEngine::submit(const Symbol& symbol, Order order) {
  return book_for(symbol).submit(order);
}

bool MatchingEngine::cancel(const Symbol& symbol, OrderId id) {
  const auto it = books_.find(symbol);
  return it != books_.end() && it->second->cancel(id);
}

const Order* MatchingEngine::find(const Symbol& symbol, OrderId id) const {
  const OrderBook* book = book_of(symbol);
  return book == nullptr ? nullptr : book->find(id);
}

bool MatchingEngine::best_bid(const Symbol& symbol, Ticks* out) const {
  const OrderBook* book = book_of(symbol);
  return book != nullptr && book->best_bid(out);
}

bool MatchingEngine::best_ask(const Symbol& symbol, Ticks* out) const {
  const OrderBook* book = book_of(symbol);
  return book != nullptr && book->best_ask(out);
}

Quantity MatchingEngine::total_qty_at(const Symbol& symbol, Side side,
                                      Ticks price) const {
  const OrderBook* book = book_of(symbol);
  return book == nullptr ? 0 : book->total_qty_at(side, price);
}

Quantity MatchingEngine::displayed_qty_at(const Symbol& symbol, Side side,
                                          Ticks price) const {
  const OrderBook* book = book_of(symbol);
  return book == nullptr ? 0 : book->displayed_qty_at(side, price);
}

std::size_t MatchingEngine::resting_count(const Symbol& symbol) const {
  const OrderBook* book = book_of(symbol);
  return book == nullptr ? 0 : book->resting_count();
}

std::vector<OrderId> MatchingEngine::expire(const Symbol& symbol,
                                            Timestamp now) {
  const auto it = books_.find(symbol);
  if (it == books_.end()) {
    return {};
  }
  return it->second->expire(now);
}

void MatchingEngine::seed_position(const Symbol& symbol, ClientId client,
                                   Quantity net) {
  book_for(symbol).seed_position(client, net);
}

Quantity MatchingEngine::position(const Symbol& symbol, ClientId client) const {
  const OrderBook* book = book_of(symbol);
  return book == nullptr ? 0 : book->position(client);
}

std::vector<Trade> MatchingEngine::run_opening_auction(const Symbol& symbol) {
  return book_for(symbol).run_opening_auction();
}

std::vector<Trade> MatchingEngine::run_closing_auction(const Symbol& symbol) {
  return book_for(symbol).run_closing_auction();
}

std::size_t MatchingEngine::auction_count(const Symbol& symbol,
                                          bool opening) const {
  const OrderBook* book = book_of(symbol);
  return book == nullptr ? 0 : book->auction_count(opening);
}

}  // namespace codicis
