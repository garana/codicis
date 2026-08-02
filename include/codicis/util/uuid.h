#ifndef CODICIS_UTIL_UUID_H
#define CODICIS_UTIL_UUID_H

/**
 * @file uuid.h
 * @brief Random (version 4) UUID generation and formatting.
 *
 * Used for external, opaque handles (order handles, user ids) that must not
 * leak internal sequential ids or ordering/rate information. Version 4 is
 * random (unlike time-sortable v7), so it exposes nothing.
 */

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace codicis {

/** @brief A 128-bit UUID. */
using Uuid = std::array<std::uint8_t, 16>;

/**
 * @brief Generates random version-4 UUIDs.
 *
 * Not thread-safe; one per thread/loop. Seed explicitly for deterministic
 * tests.
 */
class UuidGenerator {
 public:
  /** @brief Construct seeded from a non-deterministic source. */
  UuidGenerator();

  /**
   * @brief Construct with a fixed seed (deterministic, for tests).
   * @param seed The PRNG seed.
   */
  explicit UuidGenerator(std::uint64_t seed) : rng_(seed) {}

  /** @return A new version-4 UUID. */
  Uuid generate();

  /** @return A new version-4 UUID in canonical string form. */
  std::string generate_string();

 private:
  std::mt19937_64 rng_;
};

/**
 * @brief Canonical lowercase 8-4-4-4-12 hex string of a UUID.
 * @param u The UUID.
 * @return The 36-character string.
 */
std::string UuidToString(const Uuid& u);

/**
 * @brief Check that a string is a well-formed UUID (8-4-4-4-12 hex).
 * @param s The candidate string.
 * @return True if @p s has the canonical UUID shape.
 */
bool IsValidUuidString(std::string_view s);

}  // namespace codicis

#endif  // CODICIS_UTIL_UUID_H
