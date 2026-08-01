#ifndef CODICIS_CORE_ORDER_H
#define CODICIS_CORE_ORDER_H

/**
 * @file order.h
 * @brief The order model: orthogonal type / TIF / modifier axes.
 *
 * To avoid a combinatorial explosion of subclasses, an @ref codicis::Order is
 * a compact record plus a flag bitset and a few optional payload structs for
 * the conditional axes (trigger, peg, link). Any real product order is a
 * combination of these axes. An ingress @ref codicis::Normalize step collapses
 * redundant encodings (GTX to Post-Only, FOK to IOC+AON) so the matcher only
 * ever sees a canonical order.
 */

#include <cstdint>
#include <optional>

#include "codicis/core/types.h"

namespace codicis {

/** @brief Base matching behavior. Everything else is a modifier/trigger/peg. */
enum class OrdType : std::uint8_t {
  Market,
  Limit,
};

/** @brief Time-in-force (orthogonal to @ref OrdType). */
enum class Tif : std::uint8_t {
  GTC,  /**< Good-til-canceled. */
  DAY,  /**< Valid for the session. */
  GTD,  /**< Good-til-date. */
  IOC,  /**< Immediate-or-cancel. */
  FOK,  /**< Fill-or-kill (normalized to IOC + AON). */
  GTX,  /**< Post-only (normalized to GTC + Post-Only). */
};

/** @brief Freely combinable display/liquidity/execution modifiers. */
enum class OrderFlag : std::uint32_t {
  kNone = 0,
  kHidden = 1u << 0,      /**< Not shown in the public book. */
  kPostOnly = 1u << 1,    /**< Maker-only; reject if it would take. */
  kReduceOnly = 1u << 2,  /**< May only reduce a position. */
  kAon = 1u << 3,         /**< All-or-none. */
  kIceberg = 1u << 4,     /**< Only display_qty is visible. */
  kDiscretion = 1u << 5,  /**< Trades within a hidden price band. */
  kMoo = 1u << 6,         /**< Market-on-open. */
  kLoo = 1u << 7,         /**< Limit-on-open. */
  kMoc = 1u << 8,         /**< Market-on-close. */
  kLoc = 1u << 9,         /**< Limit-on-close. */
  kOpg = 1u << 10,        /**< At-the-open. */
};

/** @brief Bitwise OR of two flag masks. */
inline std::uint32_t operator|(OrderFlag a, OrderFlag b) {
  return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}

/** @return True if @p mask contains @p flag. */
inline bool HasFlag(std::uint32_t mask, OrderFlag flag) {
  return (mask & static_cast<std::uint32_t>(flag)) != 0;
}

/** @brief Set @p flag in @p mask. */
inline void SetFlag(std::uint32_t* mask, OrderFlag flag) {
  *mask |= static_cast<std::uint32_t>(flag);
}

/** @brief Stop / trailing trigger specification (conditional axis). */
struct TriggerSpec {
  enum class Kind : std::uint8_t { Stop, TrailAmount, TrailPercent };
  Kind kind = Kind::Stop;
  Ticks stop_price = 0;
  Ticks trail_by = 0;
  Ticks high_water = 0;
  bool triggered = false;
};

/** @brief Pegged-order specification (conditional axis). */
struct PegSpec {
  enum class Ref : std::uint8_t { PrimaryBid, PrimaryAsk, Opposite, Midpoint };
  Ref ref = Ref::Midpoint;
  Ticks offset = 0;
  Ticks cap_limit = 0;  /**< 0 means no cap. */
};

/** @brief Linked/contingent order specification (conditional axis). */
struct LinkSpec {
  enum class Role : std::uint8_t {
    OcoLeg,
    OtoParent,
    OtoChild,
    BracketParent,
    BracketTakeProfit,
    BracketStopLoss,
  };
  Role role = Role::OcoLeg;
  std::uint64_t group_id = 0;
};

/**
 * @brief A single order in canonical form.
 */
struct Order {
  OrderId id = 0;
  ClientId client_id = 0;   /**< 0 disables self-trade prevention. */
  Side side = Side::Buy;
  OrdType type = OrdType::Limit;
  Tif tif = Tif::GTC;
  std::uint32_t flags = 0;

  Ticks price = 0;          /**< Limit price; ignored for Market. */
  Quantity qty = 0;         /**< Original quantity. */
  Quantity leaves = 0;      /**< Remaining unfilled quantity. */
  Quantity filled = 0;      /**< Executed quantity (qty - leaves). */
  Quantity display_qty = 0; /**< Iceberg visible slice (== qty otherwise). */
  Quantity min_qty = 0;     /**< Minimum executable quantity (0 = none). */

  SeqNo seq = 0;            /**< Arrival order -> time priority. */
  Timestamp expiry_ns = 0;  /**< GTD/DAY expiry; 0 means no expiry. */

  std::optional<TriggerSpec> trigger;
  std::optional<PegSpec> peg;
  std::optional<LinkSpec> link;
};

/**
 * @brief Canonicalize redundant encodings before matching.
 *
 * GTX becomes GTC + Post-Only; FOK becomes IOC + AON. Also initializes
 * @c leaves and @c display_qty from @c qty when unset.
 * @param order The order to normalize in place.
 */
void Normalize(Order* order);

}  // namespace codicis

#endif  // CODICIS_CORE_ORDER_H
