/**
 * @file auth_client.cc
 * @brief Implementation of AuthClient (see auth_client.h).
 */

#include "codicis/auth/auth_client.h"

#include <algorithm>
#include <utility>

#include "codicis/util/uuid.h"

namespace codicis {
namespace {

/**
 * @brief Parse a decimal nanosecond count.
 * @param s   The text to parse.
 * @param out Receives the parsed value on success.
 * @return True if @p s is a valid non-negative decimal integer.
 */
bool ParseNanos(const std::string& s, Nanos* out) {
  if (s.empty()) {
    return false;
  }
  Nanos value = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

}  // namespace

AuthClient::AuthClient(const Clock& clock, Config cfg)
    : clock_(clock),
      cfg_(std::move(cfg)),
      positive_(clock, cfg_.positive_capacity, cfg_.positive_max_bytes),
      negative_(clock, cfg_.negative_capacity, cfg_.negative_max_bytes) {}

Result<std::unique_ptr<AuthClient>> AuthClient::Create(EventLoop& loop,
                                                       const Clock& clock,
                                                       Config cfg) {
  std::unique_ptr<AuthClient> c(new AuthClient(clock, std::move(cfg)));
  const std::size_t n = std::max<std::size_t>(1, c->cfg_.concurrency);
  for (std::size_t i = 0; i < n; ++i) {
    Result<std::unique_ptr<HelperClient>> hr =
        SpawnHelper(loop, c->cfg_.argv, c->codec_, c->cfg_.request_timeout_ns);
    if (!hr.ok()) {
      return hr.error();
    }
    c->helpers_.push_back(std::move(hr.value()));
  }
  c->inflight_.assign(c->helpers_.size(), 0);
  return c;
}

void AuthClient::resolve(const std::string& credential, ResolveFn cb) {
  // 1. Serve from cache (positive then negative).
  if (const std::string* uuid = positive_.lookup(credential)) {
    cb(true, *uuid);
    return;
  }
  if (negative_.lookup(credential) != nullptr) {
    cb(false, std::string());
    return;
  }

  // 2. Coalesce onto an outstanding (or queued) request for the same token.
  const auto it = waiters_.find(credential);
  if (it != waiters_.end()) {
    it->second.push_back(std::move(cb));
    return;
  }
  waiters_[credential].push_back(std::move(cb));

  // 3. Dispatch now if a helper slot is free, else queue for one.
  const int idx = free_helper();
  if (idx < 0) {
    pending_.push_back(credential);
    return;
  }
  dispatch(static_cast<std::size_t>(idx), credential);
}

int AuthClient::free_helper() {
  const std::size_t n = helpers_.size();
  for (std::size_t k = 0; k < n; ++k) {
    const std::size_t idx = (next_helper_ + k) % n;
    if (inflight_[idx] < cfg_.depth) {
      next_helper_ = (idx + 1) % n;
      return static_cast<int>(idx);
    }
  }
  return -1;
}

void AuthClient::dispatch(std::size_t idx, const std::string& credential) {
  ++inflight_[idx];
  ++helper_requests_;
  helpers_[idx]->send(
      "validate", {{"credential", credential}},
      [this, idx, credential](bool ok, const HelperMessage& reply) {
        on_response(idx, credential, ok, reply);
      });
}

void AuthClient::on_response(std::size_t idx, const std::string& credential,
                             bool ok, const HelperMessage& reply) {
  --inflight_[idx];

  bool allow = false;
  std::string user;
  if (!ok) {
    // Transport failure/timeout: deny this attempt but do NOT cache -- a
    // transient helper crash must not pin a denial.
  } else {
    const std::string* status = reply.get("status");
    const std::string* u = reply.get("user");
    if (status != nullptr && *status == "ok" && u != nullptr &&
        IsValidUuidString(*u)) {
      allow = true;
      user = *u;
      // Positive entry lives until not_after, capped by the configured TTL.
      Nanos expiry = clock_.now() + cfg_.positive_ttl_ns;
      Nanos not_after = 0;
      if (const std::string* na = reply.get("not_after");
          na != nullptr && ParseNanos(*na, &not_after)) {
        expiry = std::min(expiry, not_after);
      }
      positive_.insert(credential, user, expiry);
    } else {
      // Authoritative denial: cache it briefly.
      negative_.insert(credential, std::string(),
                       clock_.now() + cfg_.negative_ttl_ns);
    }
  }

  fan_out(credential, allow, user);
  drain_pending();
}

void AuthClient::drain_pending() {
  while (!pending_.empty()) {
    const int idx = free_helper();
    if (idx < 0) {
      break;
    }
    const std::string credential = pending_.front();
    pending_.pop_front();
    dispatch(static_cast<std::size_t>(idx), credential);
  }
}

void AuthClient::fan_out(const std::string& credential, bool ok,
                         const std::string& user_uuid) {
  const auto it = waiters_.find(credential);
  if (it == waiters_.end()) {
    return;
  }
  std::vector<ResolveFn> cbs = std::move(it->second);
  waiters_.erase(it);
  for (ResolveFn& cb : cbs) {
    if (cb) {
      cb(ok, user_uuid);
    }
  }
}

}  // namespace codicis
