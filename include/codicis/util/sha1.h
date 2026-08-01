#ifndef CODICIS_UTIL_SHA1_H
#define CODICIS_UTIL_SHA1_H

/**
 * @file sha1.h
 * @brief SHA-1 digest (RFC 3174).
 *
 * Provided for the WebSocket handshake (Sec-WebSocket-Accept). SHA-1 is not
 * used for any security-sensitive purpose here.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace codicis {

/** @brief A 20-byte SHA-1 digest. */
using Sha1Digest = std::array<std::uint8_t, 20>;

/**
 * @brief Compute the SHA-1 digest of a byte range.
 * @param data Pointer to the bytes.
 * @param len  Number of bytes.
 * @return The 20-byte digest.
 */
Sha1Digest Sha1(const std::uint8_t* data, std::size_t len);

/**
 * @brief Compute the SHA-1 digest of a string's bytes.
 * @param s The input bytes.
 * @return The 20-byte digest.
 */
Sha1Digest Sha1(const std::string& s);

/**
 * @brief Lowercase hex encoding of a SHA-1 digest.
 * @param d The digest.
 * @return A 40-character hex string.
 */
std::string Sha1Hex(const Sha1Digest& d);

}  // namespace codicis

#endif  // CODICIS_UTIL_SHA1_H
