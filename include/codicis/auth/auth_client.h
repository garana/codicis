#ifndef CODICIS_AUTH_AUTH_CLIENT_H
#define CODICIS_AUTH_AUTH_CLIENT_H

/**
 * @file auth_client.h
 * @brief Resolve a credential to a user UUID via a pool of auth helpers.
 *
 * AuthClient validates an opaque credential (typically a bearer token from a
 * request header) by asking an auth helper child process, and returns the
 * resolved user UUID asynchronously. It keeps the matching process free of the
 * (often cryptographic) validation work through three mechanisms:
 *
 *   - a POOL of helper processes so several validations run in parallel, each
 *     accepting up to a configured number of pipelined in-flight requests;
 *   - a positive + negative @ref TokenCache so a repeat credential is answered
 *     from memory (a positive entry lives until the helper's `not_after`);
 *   - single-flight COALESCING so concurrent resolves of the same credential
 *     issue only one helper request and share its response.
 *
 * All work is driven by the event loop; nothing blocks.
 */

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "codicis/auth/token_cache.h"
#include "codicis/event/event_loop.h"
#include "codicis/ipc/helper_client.h"
#include "codicis/ipc/helper_codec.h"
#include "codicis/util/clock.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief Credential -> user-UUID resolver over a pooled, cached auth helper.
 */
class AuthClient {
 public:
  /**
   * @brief Result callback.
   * @param ok        True if the credential resolved to a valid user.
   * @param user_uuid The resolved user UUID (valid only when @p ok).
   */
  using ResolveFn = std::function<void(bool ok, const std::string& user_uuid)>;

  /** @brief Construction parameters (derived from config by the caller). */
  struct Config {
    std::vector<std::string> argv;       /**< Helper command + arguments. */
    std::size_t concurrency = 1;         /**< Helper processes to spawn. */
    std::size_t depth = 1;               /**< Max in-flight per helper. */
    Nanos request_timeout_ns = 0;        /**< Per-request timeout (0=off). */
    std::size_t positive_capacity = 0;   /**< Positive cache size. */
    Nanos positive_ttl_ns = 0;           /**< Positive cache lifetime cap. */
    std::size_t negative_capacity = 0;   /**< Negative cache size. */
    Nanos negative_ttl_ns = 0;           /**< Negative cache lifetime. */
  };

  /**
   * @brief Spawn the helper pool and build a client.
   * @param loop  The event loop (must outlive the client).
   * @param clock Wall clock used for cache expiry (must outlive the client).
   * @param cfg   Pool + cache configuration.
   * @return The client, or an Error if any helper failed to spawn.
   */
  static Result<std::unique_ptr<AuthClient>> Create(EventLoop& loop,
                                                    const Clock& clock,
                                                    Config cfg);

  AuthClient(const AuthClient&) = delete;
  AuthClient& operator=(const AuthClient&) = delete;

  /**
   * @brief Resolve a credential, from cache or the helper pool.
   *
   * If the credential is cached, @p cb is invoked (still asynchronously-safe).
   * If a request for the same credential is already outstanding, this call
   * attaches to it rather than issuing another (coalescing).
   * @param credential The opaque credential to validate.
   * @param cb         Result callback.
   */
  void resolve(const std::string& credential, ResolveFn cb);

  /** @return The total number of requests actually sent to helpers. */
  std::size_t helper_requests() const { return helper_requests_; }

 private:
  AuthClient(const Clock& clock, Config cfg);

  /** @return A helper index with a free in-flight slot, or -1 if none. */
  int free_helper();

  /** @brief Send a validate request for @p credential on helper @p idx. */
  void dispatch(std::size_t idx, const std::string& credential);

  /** @brief Handle a helper response and fan it out to all waiters. */
  void on_response(std::size_t idx, const std::string& credential, bool ok,
                   const HelperMessage& reply);

  /** @brief Start queued credentials while helper slots are free. */
  void drain_pending();

  /** @brief Deliver a result to every waiter on @p credential. */
  void fan_out(const std::string& credential, bool ok,
               const std::string& user_uuid);

  const Clock& clock_;
  Config cfg_;
  TextHelperCodec codec_;
  TokenCache positive_;
  TokenCache negative_;

  std::vector<std::unique_ptr<HelperClient>> helpers_;
  std::vector<std::size_t> inflight_;  // per-helper in-flight request count
  std::size_t next_helper_ = 0;        // round-robin cursor

  // Coalescing: credential -> callbacks awaiting one shared response.
  std::unordered_map<std::string, std::vector<ResolveFn>> waiters_;
  // Credentials waiting for a free helper slot (each also present in waiters_).
  std::deque<std::string> pending_;

  std::size_t helper_requests_ = 0;
};

}  // namespace codicis

#endif  // CODICIS_AUTH_AUTH_CLIENT_H
