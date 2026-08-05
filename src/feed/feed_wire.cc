/**
 * @file feed_wire.cc
 * @brief Implementation of the book-event feed wire codec (see feed_wire.h).
 */

#include "codicis/feed/feed_wire.h"

#include <cstring>

namespace codicis {
namespace {

/** @brief Append a little-endian unsigned integer of @p n bytes. */
void PutUint(std::string* out, std::uint64_t v, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

/** @brief Read a little-endian unsigned integer of @p n bytes from @p p. */
std::uint64_t GetUint(const std::uint8_t* p, std::size_t n) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

/** @brief Largest defined BookEventType tag (for validation). */
constexpr std::uint8_t kMaxType =
    static_cast<std::uint8_t>(BookEventType::kTrigger);

}  // namespace

void EncodeBookEvent(const BookEvent& ev, std::string* out) {
  out->push_back(static_cast<char>(static_cast<std::uint8_t>(ev.type)));
  out->push_back(static_cast<char>(ev.side == Side::Sell ? 1 : 0));
  PutUint(out, static_cast<std::uint16_t>(ev.symbol.size()), 2);
  PutUint(out, ev.seq, 8);
  PutUint(out, ev.order_id, 8);
  PutUint(out, ev.taker_id, 8);
  PutUint(out, static_cast<std::uint64_t>(ev.price), 8);
  PutUint(out, static_cast<std::uint64_t>(ev.prev_price), 8);
  PutUint(out, static_cast<std::uint64_t>(ev.qty), 8);
  PutUint(out, static_cast<std::uint64_t>(ev.displayed), 8);
  out->append(ev.symbol);
}

FeedDecode DecodeBookEvent(Buffer& in, BookEvent* out) {
  const std::string_view v = in.view();
  if (v.size() < kFeedRecordFixed) {
    return FeedDecode::kIncomplete;
  }
  const auto* base = reinterpret_cast<const std::uint8_t*>(v.data());
  const auto type = static_cast<std::uint8_t>(base[0]);
  if (type > kMaxType) {
    return FeedDecode::kError;
  }
  const auto side = static_cast<std::uint8_t>(base[1]);
  const std::size_t symbol_len = static_cast<std::size_t>(GetUint(base + 2, 2));
  const std::size_t total = kFeedRecordFixed + symbol_len;
  if (v.size() < total) {
    return FeedDecode::kIncomplete;
  }

  out->type = static_cast<BookEventType>(type);
  out->side = side == 1 ? Side::Sell : Side::Buy;
  out->seq = GetUint(base + 4, 8);
  out->order_id = GetUint(base + 12, 8);
  out->taker_id = GetUint(base + 20, 8);
  out->price = static_cast<Ticks>(GetUint(base + 28, 8));
  out->prev_price = static_cast<Ticks>(GetUint(base + 36, 8));
  out->qty = static_cast<Quantity>(GetUint(base + 44, 8));
  out->displayed = static_cast<Quantity>(GetUint(base + 52, 8));
  out->symbol.assign(reinterpret_cast<const char*>(base + kFeedRecordFixed),
                     symbol_len);
  in.consume(total);
  return FeedDecode::kComplete;
}

}  // namespace codicis
