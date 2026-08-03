/**
 * @file test_auth.cc
 * @brief Tests for AuthClient over the spawned reference auth helper.
 */

#include "catch_amalgamated.hpp"

#include "codicis/auth/auth_client.h"
#include "codicis/event/event_loop.h"
#include "codicis/util/clock.h"

#include <memory>
#include <string>

using namespace codicis;

namespace {

/** @brief Build an AuthClient over the reference helper (spawned N times). */
std::unique_ptr<AuthClient> MakeClient(EventLoop& loop, const Clock& clock,
                                       std::size_t concurrency,
                                       std::size_t depth) {
  AuthClient::Config cfg;
  cfg.argv = {CODICIS_AUTH_HELPER_PATH};
  cfg.concurrency = concurrency;
  cfg.depth = depth;
  cfg.request_timeout_ns = 0;
  cfg.positive_capacity = 64;
  cfg.positive_ttl_ns = 1'000'000'000;  // 1 s in the cache clock's domain
  cfg.negative_capacity = 64;
  cfg.negative_ttl_ns = 1'000'000'000;
  Result<std::unique_ptr<AuthClient>> r =
      AuthClient::Create(loop, clock, std::move(cfg));
  REQUIRE(r.ok());
  return std::move(r.value());
}

/** @brief A valid v4 UUID for tests. */
const std::string kUser = "11111111-1111-4111-8111-111111111111";

}  // namespace

TEST_CASE("AuthClient resolves and denies credentials", "[auth][client]") {
  SystemClock loop_clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&loop_clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();
  ManualClock cache_clock(1000);
  auto client = MakeClient(loop, cache_clock, /*concurrency=*/2, /*depth=*/4);

  auto run_until = [&](const bool& done) {
    for (int i = 0; i < 200 && !done; ++i) {
      loop.run_once(5);
    }
  };

  SECTION("a valid credential resolves to its user") {
    bool done = false, ok = false;
    std::string uuid;
    client->resolve(kUser + ":good", [&](bool o, const std::string& u) {
      done = true;
      ok = o;
      uuid = u;
    });
    run_until(done);
    REQUIRE(done);
    REQUIRE(ok);
    REQUIRE(uuid == kUser);
  }

  SECTION("a bad secret is denied") {
    bool done = false, ok = true;
    client->resolve(kUser + ":bad", [&](bool o, const std::string&) {
      done = true;
      ok = o;
    });
    run_until(done);
    REQUIRE(done);
    REQUIRE_FALSE(ok);
  }

  SECTION("a repeat credential is served from cache (no new request)") {
    bool done = false;
    client->resolve(kUser + ":good",
                    [&](bool, const std::string&) { done = true; });
    run_until(done);
    const std::size_t after_first = client->helper_requests();

    bool done2 = false, ok2 = false;
    std::string uuid2;
    client->resolve(kUser + ":good", [&](bool o, const std::string& u) {
      done2 = true;
      ok2 = o;
      uuid2 = u;
    });
    REQUIRE(done2);  // cache hit answers synchronously
    REQUIRE(ok2);
    REQUIRE(uuid2 == kUser);
    REQUIRE(client->helper_requests() == after_first);  // no extra request
  }

  SECTION("concurrent resolves of one token are coalesced") {
    const std::string cred =
        "22222222-2222-4222-8222-222222222222:good";  // fresh, uncached
    const std::size_t before = client->helper_requests();
    bool a_done = false, b_done = false, a_ok = false, b_ok = false;
    std::string a_uuid, b_uuid;
    client->resolve(cred, [&](bool o, const std::string& u) {
      a_done = true;
      a_ok = o;
      a_uuid = u;
    });
    client->resolve(cred, [&](bool o, const std::string& u) {
      b_done = true;
      b_ok = o;
      b_uuid = u;
    });
    // Only one helper request issued for the two outstanding resolves.
    REQUIRE(client->helper_requests() == before + 1);

    for (int i = 0; i < 200 && !(a_done && b_done); ++i) {
      loop.run_once(5);
    }
    REQUIRE(a_done);
    REQUIRE(b_done);
    REQUIRE(a_ok);
    REQUIRE(b_ok);
    REQUIRE(a_uuid == b_uuid);
    REQUIRE(a_uuid == "22222222-2222-4222-8222-222222222222");
  }
}
