/**
 * @file test_token_cache.cc
 * @brief Unit tests for the auth TokenCache (approximate weighted-LRU).
 */

#include "catch_amalgamated.hpp"

#include "codicis/auth/token_cache.h"
#include "codicis/util/clock.h"

using namespace codicis;

namespace {
constexpr Nanos kFar = 1'000'000;  // a comfortably-future absolute expiry
}  // namespace

TEST_CASE("TokenCache hit and miss", "[auth][cache]") {
  ManualClock clock(0);
  TokenCache cache(clock, /*capacity=*/8);

  REQUIRE(cache.lookup("absent") == nullptr);

  cache.insert("alice", "uuid-a", kFar);
  const std::string* v = cache.lookup("alice");
  REQUIRE(v != nullptr);
  REQUIRE(*v == "uuid-a");
  REQUIRE(cache.size() == 1);
}

TEST_CASE("TokenCache stores negative (empty-value) entries", "[auth][cache]") {
  ManualClock clock(0);
  TokenCache cache(clock, 8);
  cache.insert("bad", "", kFar);  // a denial: present, value ""
  const std::string* v = cache.lookup("bad");
  REQUIRE(v != nullptr);  // present...
  REQUIRE(v->empty());    // ...but denied (distinct from an absent miss)
}

TEST_CASE("TokenCache expires entries lazily on read", "[auth][cache]") {
  ManualClock clock(0);
  TokenCache cache(clock, 8);
  cache.insert("k", "v", /*expiry_ns=*/100);
  clock.set(99);
  REQUIRE(cache.lookup("k") != nullptr);  // still live
  clock.set(100);                         // now >= expiry
  REQUIRE(cache.lookup("k") == nullptr);  // expired -> miss
  REQUIRE(cache.size() == 0);             // and evicted

  // Inserting an already-expired entry is a no-op.
  cache.insert("past", "v", 100);
  REQUIRE(cache.size() == 0);
}

TEST_CASE("TokenCache evicts from the tail at capacity", "[auth][cache]") {
  ManualClock clock(0);
  TokenCache cache(clock, /*capacity=*/2);
  cache.insert("a", "va", kFar);  // list: [a]
  cache.insert("b", "vb", kFar);  // list: [a, b]  (a at head, b at tail)
  cache.insert("c", "vc", kFar);  // full: evict tail b, admit c -> [a, c]

  REQUIRE(cache.lookup("b") == nullptr);  // the tail was evicted
  REQUIRE(cache.lookup("a") != nullptr);  // the head survived
  REQUIRE(cache.lookup("c") != nullptr);  // the newcomer is resident
  REQUIRE(cache.size() == 2);
}

TEST_CASE("TokenCache read promotes an entry away from eviction",
          "[auth][cache]") {
  ManualClock clock(0);
  TokenCache cache(clock, /*capacity=*/3);
  cache.insert("k1", "1", kFar);  // [k1]
  cache.insert("k2", "2", kFar);  // [k1, k2]
  cache.insert("k3", "3", kFar);  // [k1, k2, k3]  tail=k3

  cache.lookup("k3");  // promote k3 one step toward head -> [k1, k3, k2]

  cache.insert("k4", "4", kFar);  // full: evict tail (now k2) -> [k1, k3, k4]

  REQUIRE(cache.lookup("k2") == nullptr);  // demoted to tail, then evicted
  REQUIRE(cache.lookup("k1") != nullptr);
  REQUIRE(cache.lookup("k3") != nullptr);  // saved by the read
  REQUIRE(cache.lookup("k4") != nullptr);
}

TEST_CASE("TokenCache with zero capacity is disabled", "[auth][cache]") {
  ManualClock clock(0);
  TokenCache cache(clock, /*capacity=*/0);
  cache.insert("k", "v", kFar);
  REQUIRE(cache.lookup("k") == nullptr);
  REQUIRE(cache.size() == 0);
}
