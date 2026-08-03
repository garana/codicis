#ifndef CODICIS_AUTH_TOKEN_CACHE_H
#define CODICIS_AUTH_TOKEN_CACHE_H

/**
 * @file token_cache.h
 * @brief A small time-bounded credential cache with approximate-LRU eviction.
 *
 * TokenCache maps an opaque credential string to a resolved value (a user
 * UUID, or the empty string for a negative/"denied" cache) with a per-entry
 * absolute expiry supplied by the caller (a positive entry expires at the auth
 * helper's `not_after`; a negative entry at now + a short TTL). It is an
 * approximate, "weighted" LRU built on an intrusive doubly linked list:
 *
 *   - a new entry is pushed at the TAIL (the cold, probationary end);
 *   - a read promotes its entry one step toward the HEAD (the hot end), so an
 *     entry that is read repeatedly drifts away from eviction -- its position
 *     is thus weighted by how often it is used;
 *   - when the cache is over its entry-count OR byte budget, the entry at the
 *     TAIL is evicted (repeatedly) to make room.
 *
 * Admitting new entries at the cold end means a one-off credential cannot push
 * the hot set out on a single miss: it must be read again to be protected.
 * TTL bounds staleness independently of position. Single-threaded; the caller
 * (the event loop) serializes access. Time is read through an injected Clock so
 * tests can drive expiry deterministically.
 *
 * Expiry is lazy: an entry is dropped when a lookup finds it expired, and each
 * insert first purges expired entries at the tail (the cold end) -- so a dead
 * entry is reclaimed before a live one is evicted to satisfy a size or byte
 * budget. The purge stops at the first non-expired tail entry and at a
 * configurable per-insert cap, so a single insert never walks the whole list.
 * This is opportunistic, not a full sweep: an expired entry stranded near the
 * hot head is only reclaimed when it is next looked up.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

#include "codicis/util/clock.h"

namespace codicis {

/**
 * @brief Time-bounded credential -> value cache with approximate-LRU eviction.
 */
class TokenCache {
 public:
  /**
   * @brief Construct a cache.
   * @param clock       Clock read to test entry expiry (must outlive this). Use
   *                    a wall clock when expiries are absolute wall times.
   * @param capacity    Maximum number of resident entries (0 disables the
   *                    cache).
   * @param max_bytes   Maximum total payload bytes (sum of key + value sizes)
   *                    across resident entries; 0 means no byte budget. An
   *                    entry larger than the whole budget is never cached.
   * @param max_purge   Maximum expired entries reclaimed from the tail per
   *                    insert; 0 means unbounded. Bounds the per-insert work of
   *                    opportunistic purging, so one insert cannot pay to
   *                    reclaim an entire cache of expired entries.
   */
  TokenCache(const Clock& clock, std::size_t capacity, std::size_t max_bytes = 0,
             std::size_t max_purge = 0);

  ~TokenCache();

  TokenCache(const TokenCache&) = delete;
  TokenCache& operator=(const TokenCache&) = delete;

  /**
   * @brief Insert or replace an entry, refreshing its expiry and position.
   *
   * A no-op if the cache is disabled (capacity 0) or the expiry is already in
   * the past. If inserting a new key would exceed capacity, the tail entry is
   * evicted first.
   * @param key       The credential.
   * @param value     The resolved value (a user UUID, or "" for a negative
   *                  entry).
   * @param expiry_ns Absolute time (in the cache clock's domain) after which
   *                  the entry is invalid.
   */
  void insert(const std::string& key, const std::string& value,
              Nanos expiry_ns);

  /**
   * @brief Look up a live entry, promoting it one step toward the head.
   *
   * An expired entry is erased and reported as a miss.
   * @param key The credential to look up.
   * @return Pointer to the stored value (possibly ""), or nullptr on a miss.
   *         The pointer is valid until the next mutating call.
   */
  const std::string* lookup(const std::string& key);

  /** @return The number of resident entries. */
  std::size_t size() const { return index_.size(); }

  /** @return The total payload bytes (sum of key + value sizes) resident. */
  std::size_t bytes() const { return bytes_; }

 private:
  /** @brief One cache entry, linked into the intrusive LRU list. */
  struct Node {
    std::string key;
    std::string value;
    Nanos expiry = 0;
    Node* prev = nullptr;
    Node* next = nullptr;
  };

  /** @brief Unlink @p n from the list (does not free it). */
  void unlink(Node* n);

  /** @brief Append @p n at the tail (the cold end). */
  void push_tail(Node* n);

  /** @brief Erase @p n from both the list and the index. */
  void erase(Node* n);

  /** @brief Drop expired entries from the tail (the cold end). */
  void purge_expired_tail();

  const Clock& clock_;
  const std::size_t capacity_;
  const std::size_t max_bytes_;
  const std::size_t max_purge_;

  std::unordered_map<std::string, std::unique_ptr<Node>> index_;
  Node* head_ = nullptr;  // hot end (promoted, protected)
  Node* tail_ = nullptr;  // cold end (admission + eviction)
  std::size_t bytes_ = 0;  // resident payload bytes (sum of key + value sizes)
};

}  // namespace codicis

#endif  // CODICIS_AUTH_TOKEN_CACHE_H
