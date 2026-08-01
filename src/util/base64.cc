/**
 * @file base64.cc
 * @brief Implementation of Base64 encoding (see base64.h).
 */

#include "codicis/util/base64.h"

namespace codicis {
namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string Base64Encode(const std::uint8_t* data, std::size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= len) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                            static_cast<std::uint32_t>(data[i + 2]);
    out.push_back(kAlphabet[(n >> 18) & 0x3F]);
    out.push_back(kAlphabet[(n >> 12) & 0x3F]);
    out.push_back(kAlphabet[(n >> 6) & 0x3F]);
    out.push_back(kAlphabet[n & 0x3F]);
    i += 3;
  }
  const std::size_t rem = len - i;
  if (rem == 1) {
    const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
    out.push_back(kAlphabet[(n >> 18) & 0x3F]);
    out.push_back(kAlphabet[(n >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                            (static_cast<std::uint32_t>(data[i + 1]) << 8);
    out.push_back(kAlphabet[(n >> 18) & 0x3F]);
    out.push_back(kAlphabet[(n >> 12) & 0x3F]);
    out.push_back(kAlphabet[(n >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::string Base64Encode(const std::string& s) {
  return Base64Encode(reinterpret_cast<const std::uint8_t*>(s.data()),
                      s.size());
}

}  // namespace codicis
