/**
 * @file test_util.cc
 * @brief Unit tests for the codicis_util subsystem.
 */

#include "catch_amalgamated.hpp"

#include "codicis/util/base64.h"
#include "codicis/util/buffer.h"
#include "codicis/util/clock.h"
#include "codicis/util/logging.h"
#include "codicis/util/result.h"
#include "codicis/util/sha1.h"
#include "codicis/util/uuid.h"

#include <set>

#include <cstring>
#include <string>

using namespace codicis;

TEST_CASE("Result holds a value or an error", "[util][result]") {
  SECTION("value") {
    Result<int> r(42);
    REQUIRE(r.ok());
    REQUIRE(static_cast<bool>(r));
    REQUIRE(r.value() == 42);
  }
  SECTION("error") {
    Result<int> r(MakeError(ErrorCode::kNotFound, "missing"));
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == ErrorCode::kNotFound);
    REQUIRE(r.error().message == "missing");
  }
}

TEST_CASE("Status distinguishes success from failure", "[util][result]") {
  REQUIRE(Status::Ok().ok());
  Status s(MakeError(ErrorCode::kIo, "disk"));
  REQUIRE_FALSE(s.ok());
  REQUIRE(s.error().code == ErrorCode::kIo);
}

TEST_CASE("Buffer append/consume preserves FIFO order", "[util][buffer]") {
  Buffer b;
  REQUIRE(b.empty());

  b.append(std::string_view("hello "));
  b.append(std::string_view("world"));
  REQUIRE(b.size() == 11);
  REQUIRE(b.view() == "hello world");

  b.consume(6);
  REQUIRE(b.size() == 5);
  REQUIRE(b.view() == "world");

  b.consume(100);  // over-consume clamps to empty
  REQUIRE(b.empty());
}

TEST_CASE("Buffer reserve/commit supports scatter-free writes",
          "[util][buffer]") {
  Buffer b;
  const std::string src = "abcdef";
  std::uint8_t* dst = b.reserve(src.size());
  std::memcpy(dst, src.data(), src.size());
  b.commit(src.size());
  REQUIRE(b.view() == "abcdef");
}

TEST_CASE("Buffer survives grow-after-partial-consume", "[util][buffer]") {
  Buffer b;
  b.append(std::string_view("0123456789"));
  b.consume(7);              // leaves "789"
  b.append(std::string_view("ABCDEFGHIJ"));  // forces compaction/grow
  REQUIRE(b.view() == "789ABCDEFGHIJ");
}

TEST_CASE("ManualClock advances deterministically", "[util][clock]") {
  ManualClock clock(1000);
  REQUIRE(clock.now() == 1000);
  clock.advance(500);
  REQUIRE(clock.now() == 1500);
  clock.set(42);
  REQUIRE(clock.now() == 42);
}

TEST_CASE("Base64 encodes known vectors", "[util][base64]") {
  REQUIRE(Base64Encode(std::string("")) == "");
  REQUIRE(Base64Encode(std::string("M")) == "TQ==");
  REQUIRE(Base64Encode(std::string("Ma")) == "TWE=");
  REQUIRE(Base64Encode(std::string("Man")) == "TWFu");
  REQUIRE(Base64Encode(std::string("any carnal pleasure.")) ==
          "YW55IGNhcm5hbCBwbGVhc3VyZS4=");
}

TEST_CASE("SHA-1 matches RFC 3174 vectors", "[util][sha1]") {
  REQUIRE(Sha1Hex(Sha1(std::string("abc"))) ==
          "a9993e364706816aba3e25717850c26c9cd0d89d");
  REQUIRE(Sha1Hex(Sha1(std::string(""))) ==
          "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  REQUIRE(Sha1Hex(Sha1(std::string(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST_CASE("UUID v4 generation and validation", "[util][uuid]") {
  UuidGenerator gen(12345);  // fixed seed for a deterministic test
  const std::string a = gen.generate_string();
  const std::string b = gen.generate_string();

  REQUIRE(a.size() == 36);
  REQUIRE(IsValidUuidString(a));
  REQUIRE(a[14] == '4');   // version nibble
  REQUIRE((a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b'));
  REQUIRE(a != b);         // distinct

  REQUIRE_FALSE(IsValidUuidString("not-a-uuid"));
  REQUIRE_FALSE(IsValidUuidString(""));
  REQUIRE_FALSE(IsValidUuidString(std::string(36, 'z')));  // right length, bad

  // A batch is unique.
  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    seen.insert(gen.generate_string());
  }
  REQUIRE(seen.size() == 1000);
}

TEST_CASE("Log level parsing and thresholding", "[util][logging]") {
  LogLevel level;
  REQUIRE(ParseLogLevel("warn", &level));
  REQUIRE(level == LogLevel::kWarn);
  REQUIRE(ParseLogLevel("ERROR", &level));
  REQUIRE(level == LogLevel::kError);
  REQUIRE_FALSE(ParseLogLevel("bogus", &level));

  SetLogLevel(LogLevel::kWarn);
  REQUIRE(GetLogLevel() == LogLevel::kWarn);
  REQUIRE_FALSE(LogEnabled(LogLevel::kInfo));
  REQUIRE(LogEnabled(LogLevel::kError));
  SetLogLevel(LogLevel::kInfo);  // restore default
}
