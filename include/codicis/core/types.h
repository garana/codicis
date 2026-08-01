#ifndef CODICIS_CORE_TYPES_H
#define CODICIS_CORE_TYPES_H

/**
 * @file types.h
 * @brief Fundamental value types for the matching engine.
 *
 * Prices are integer ticks (never floating point) so equality and ordering
 * are exact. Order time-priority uses a monotonic arrival sequence number,
 * not wall-clock time.
 */

#include <cstdint>

namespace codicis {

/** @brief A price in integer tick units. */
using Ticks = std::int64_t;

/** @brief A quantity in integer lot/share units. */
using Quantity = std::int64_t;

/** @brief An engine-assigned, monotonically increasing order identifier. */
using OrderId = std::uint64_t;

/** @brief A monotonic arrival sequence number establishing time priority. */
using SeqNo = std::uint64_t;

/** @brief The side of an order. */
enum class Side : std::uint8_t {
  Buy,
  Sell,
};

/** @return The opposite side of @p s. */
inline Side Opposite(Side s) {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace codicis

#endif  // CODICIS_CORE_TYPES_H
