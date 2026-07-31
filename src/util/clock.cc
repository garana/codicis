/**
 * @file clock.cc
 * @brief Implementation of SystemClock (see clock.h).
 */

#include "codicis/util/clock.h"

#include <chrono>

namespace codicis {

Nanos SystemClock::now() const {
  const auto t = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t).count();
}

}  // namespace codicis
