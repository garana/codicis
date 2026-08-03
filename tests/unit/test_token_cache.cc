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

TEST_CASE("TokenCache enforces a byte budget", "[auth][cache]") {
  ManualClock clock(0);
  // Generous entry cap; the byte budget is the binding constraint. Each entry
  // below is key(2) + value(3) = 5 bytes, so a 12-byte budget holds two.
  TokenCache cache(clock, /*capacity=*/100, /*max_bytes=*/12);

  cache.insert("k1", "aaa", kFar);  // [k1]
  cache.insert("k2", "bbb", kFar);  // [k1(head), k2(tail)]
  REQUIRE(cache.size() == 2);
  REQUIRE(cache.bytes() == 10);

  cache.insert("k3", "ccc", kFar);  // 10+5 > 12: evict the tail (k2) to fit
  REQUIRE(cache.size() == 2);
  REQUIRE(cache.bytes() == 10);
  REQUIRE(cache.lookup("k2") == nullptr);  // tail (cold end) evicted
  REQUIRE(cache.lookup("k1") != nullptr);  // head survived
  REQUIRE(cache.lookup("k3") != nullptr);

  // An entry larger than the whole budget is never cached.
  cache.insert("big", std::string(100, 'x'), kFar);
  REQUIRE(cache.lookup("big") == nullptr);

  // Eviction keeps the byte counter exact.
  REQUIRE(cache.bytes() <= 12);
}

TEST_CASE("TokenCache insert purges expired entries from the tail",
          "[auth][cache]") {
  ManualClock clock(0);
  TokenCache cache(clock, /*capacity=*/100);  // count is not the constraint
  cache.insert("a", "1", /*expiry_ns=*/10);   // [a]
  cache.insert("b", "2", /*expiry_ns=*/10);   // [a(head), b(tail)]
  REQUIRE(cache.size() == 2);

  clock.set(10);  // a and b are now expired (but still resident)

  // A later insert reclaims the dead tail entries before admitting the new one,
  // even though the entry-count cap was never reached.
  cache.insert("c", "3", kFar);
  REQUIRE(cache.size() == 1);
  REQUIRE(cache.lookup("a") == nullptr);
  REQUIRE(cache.lookup("b") == nullptr);
  REQUIRE(cache.lookup("c") != nullptr);

  // A live entry ahead of an expired tail is not purged past.
  clock.set(20);
  cache.insert("d", "4", /*expiry_ns=*/25);  // [c(head), d(tail)], c live
  clock.set(26);                             // d expired, c still live
  cache.insert("e", "5", kFar);              // purges d, keeps c
  REQUIRE(cache.lookup("d") == nullptr);
  REQUIRE(cache.lookup("c") != nullptr);
}

TEST_CASE("TokenCache caps expired-tail purging per insert", "[auth][cache]") {
  ManualClock clock(0);
  // Large caps so only the purge cap is exercised; reclaim <=1 expired/insert.
  TokenCache cache(clock, /*capacity=*/100, /*max_bytes=*/0, /*max_purge=*/1);
  cache.insert("a", "1", /*expiry_ns=*/10);  // [a]
  cache.insert("b", "2", /*expiry_ns=*/10);  // [a, b]
  cache.insert("c", "3", /*expiry_ns=*/10);  // [a, b, c(tail)]
  REQUIRE(cache.size() == 3);

  clock.set(10);  // a, b, c all expired

  // With the cap at 1, this insert reclaims only the single tail entry (c),
  // not the whole expired run -- so a and b remain resident and d is added.
  cache.insert("d", "4", kFar);
  REQUIRE(cache.size() == 3);              // 3 - 1 purged + 1 added
  REQUIRE(cache.lookup("d") != nullptr);  // the newcomer is live
}

TEST_CASE("TokenCache with zero capacity is disabled", "[auth][cache]") {
  ManualClock clock(0);
  TokenCache cache(clock, /*capacity=*/0);
  cache.insert("k", "v", kFar);
  REQUIRE(cache.lookup("k") == nullptr);
  REQUIRE(cache.size() == 0);
}
