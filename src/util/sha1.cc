/**
 * @file sha1.cc
 * @brief Implementation of SHA-1 (see sha1.h), per RFC 3174.
 */

#include "codicis/util/sha1.h"

#include <cstring>

namespace codicis {
namespace {

/** @brief Left-rotate a 32-bit word by @p bits. */
inline std::uint32_t Rotl(std::uint32_t v, unsigned bits) {
  return (v << bits) | (v >> (32 - bits));
}

}  // namespace

Sha1Digest Sha1(const std::uint8_t* data, std::size_t len) {
  std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
                h3 = 0x10325476, h4 = 0xC3D2E1F0;

  // Build the padded message: original || 0x80 || 0x00.. || 64-bit bit length.
  const std::uint64_t bit_len = static_cast<std::uint64_t>(len) * 8;
  std::size_t total = len + 1;
  while (total % 64 != 56) {
    ++total;
  }
  total += 8;

  std::string msg;
  msg.resize(total, '\0');
  std::memcpy(msg.data(), data, len);
  msg[len] = static_cast<char>(0x80);
  for (int i = 0; i < 8; ++i) {
    msg[total - 1 - static_cast<std::size_t>(i)] =
        static_cast<char>((bit_len >> (8 * i)) & 0xFF);
  }

  const auto* bytes = reinterpret_cast<const std::uint8_t*>(msg.data());
  for (std::size_t off = 0; off < total; off += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const std::size_t j = off + static_cast<std::size_t>(i) * 4;
      w[i] = (static_cast<std::uint32_t>(bytes[j]) << 24) |
             (static_cast<std::uint32_t>(bytes[j + 1]) << 16) |
             (static_cast<std::uint32_t>(bytes[j + 2]) << 8) |
             static_cast<std::uint32_t>(bytes[j + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f;
      std::uint32_t k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const std::uint32_t tmp =
          Rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = Rotl(b, 30);
      b = a;
      a = tmp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  Sha1Digest out;
  const std::uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    out[static_cast<std::size_t>(i) * 4 + 0] =
        static_cast<std::uint8_t>((hs[i] >> 24) & 0xFF);
    out[static_cast<std::size_t>(i) * 4 + 1] =
        static_cast<std::uint8_t>((hs[i] >> 16) & 0xFF);
    out[static_cast<std::size_t>(i) * 4 + 2] =
        static_cast<std::uint8_t>((hs[i] >> 8) & 0xFF);
    out[static_cast<std::size_t>(i) * 4 + 3] =
        static_cast<std::uint8_t>(hs[i] & 0xFF);
  }
  return out;
}

Sha1Digest Sha1(const std::string& s) {
  return Sha1(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

std::string Sha1Hex(const Sha1Digest& d) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(40);
  for (std::uint8_t byte : d) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

}  // namespace codicis
