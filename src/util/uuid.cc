/**
 * @file uuid.cc
 * @brief Implementation of UUID v4 generation and formatting (see uuid.h).
 */

#include "codicis/util/uuid.h"

#include <cstring>

namespace codicis {
namespace {

/** @brief True if @p c is an ASCII hex digit. */
bool IsHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

}  // namespace

UuidGenerator::UuidGenerator() : rng_(std::random_device{}()) {}

Uuid UuidGenerator::generate() {
  Uuid u{};
  const std::uint64_t hi = rng_();
  const std::uint64_t lo = rng_();
  std::memcpy(u.data(), &hi, 8);
  std::memcpy(u.data() + 8, &lo, 8);
  u[6] = static_cast<std::uint8_t>((u[6] & 0x0F) | 0x40);  // version 4
  u[8] = static_cast<std::uint8_t>((u[8] & 0x3F) | 0x80);  // variant 10xx
  return u;
}

std::string UuidGenerator::generate_string() {
  return UuidToString(generate());
}

std::string UuidToString(const Uuid& u) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (std::size_t i = 0; i < u.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      s.push_back('-');
    }
    s.push_back(kHex[u[i] >> 4]);
    s.push_back(kHex[u[i] & 0x0F]);
  }
  return s;
}

bool IsValidUuidString(std::string_view s) {
  if (s.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') {
        return false;
      }
    } else if (!IsHex(s[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace codicis
