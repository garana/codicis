#ifndef CODICIS_FEED_FEED_WIRE_H
#define CODICIS_FEED_FEED_WIRE_H

/**
 * @file feed_wire.h
 * @brief Compact binary wire encoding for the book-event feed stream.
 *
 * The matching process serializes each @ref BookEvent with @ref EncodeBookEvent
 * and writes it to the feed-helper (over a pipe today, an SPMC ring or UDP
 * later). The helper decodes the stream incrementally with @ref DecodeBookEvent.
 * The layout is fixed little-endian scalars plus a length-prefixed symbol, so a
 * decoder needs no schema and the hot path is a handful of loads.
 *
 * Wire record (little-endian):
 *   u8  type          (BookEventType)
 *   u8  side           (0 = Buy, 1 = Sell)
 *   u16 symbol_len
 *   u64 seq
 *   u64 order_id
 *   u64 taker_id
 *   i64 price
 *   i64 prev_price
 *   i64 qty
 *   u8[symbol_len] symbol
 */

#include <cstddef>
#include <cstdint>
#include <string>

#include "codicis/core/book_event.h"
#include "codicis/util/buffer.h"

namespace codicis {

/** @brief Fixed-size prefix of a wire record (before the symbol bytes). */
constexpr std::size_t kFeedRecordFixed = 1 + 1 + 2 + 8 + 8 + 8 + 8 + 8 + 8;

/** @brief Outcome of @ref DecodeBookEvent. */
enum class FeedDecode {
  kIncomplete, /**< Need more bytes; nothing consumed. */
  kComplete,   /**< One event decoded and consumed. */
  kError,      /**< Malformed record; the stream is unusable. */
};

/**
 * @brief Append the wire encoding of @p ev to @p out.
 * @param ev  The event to encode.
 * @param out Receives the appended bytes.
 */
void EncodeBookEvent(const BookEvent& ev, std::string* out);

/**
 * @brief Decode one event from @p in, consuming it on success.
 * @param in  The receive buffer; consumed only on kComplete.
 * @param out Receives the decoded event on kComplete.
 * @return The decode outcome.
 */
FeedDecode DecodeBookEvent(Buffer& in, BookEvent* out);

}  // namespace codicis

#endif  // CODICIS_FEED_FEED_WIRE_H
