#ifndef CODICIS_UTIL_CLOCK_H
#define CODICIS_UTIL_CLOCK_H

/**
 * @file clock.h
 * @brief Injectable monotonic clock abstraction.
 *
 * Time-dependent logic (IPC timeouts, WebSocket pings, GTD/DAY expiry) reads
 * the current time through a @ref codicis::Clock so tests can drive it
 * deterministically with a @ref codicis::ManualClock instead of wall time.
 *
 * Note: order time-priority does NOT use wall time; it uses a monotonic
 * arrival sequence number assigned by the matching engine.
 */

#include <cstdint>

namespace codicis {

/** @brief Monotonic nanoseconds since an unspecified but fixed epoch. */
using Nanos = std::int64_t;

/**
 * @brief Interface for reading monotonic time.
 */
class Clock {
 public:
  virtual ~Clock() = default;

  /** @return The current monotonic time in nanoseconds. */
  virtual Nanos now() const = 0;
};

/**
 * @brief A @ref Clock backed by the system steady clock.
 */
class SystemClock final : public Clock {
 public:
  Nanos now() const override;
};

/**
 * @brief A @ref Clock backed by the system wall clock (Unix epoch).
 *
 * Unlike @ref SystemClock (monotonic), this returns nanoseconds since the Unix
 * epoch, so it can be compared against absolute timestamps received from other
 * systems (e.g. an auth helper's `not_after`). Not for time-priority or
 * interval timing (it can jump); use it only for absolute expiry comparisons.
 */
class WallClock final : public Clock {
 public:
  Nanos now() const override;
};

/**
 * @brief A @ref Clock whose value is controlled explicitly, for tests.
 */
class ManualClock final : public Clock {
 public:
  /**
   * @brief Construct with an initial time.
   * @param start Initial value returned by now().
   */
  explicit ManualClock(Nanos start = 0) : now_(start) {}

  Nanos now() const override { return now_; }

  /**
   * @brief Set the current time.
   * @param t New value returned by now().
   */
  void set(Nanos t) { now_ = t; }

  /**
   * @brief Advance the current time by @p delta nanoseconds.
   * @param delta Amount to add to the current time.
   */
  void advance(Nanos delta) { now_ += delta; }

 private:
  Nanos now_;
};

}  // namespace codicis

#endif  // CODICIS_UTIL_CLOCK_H
