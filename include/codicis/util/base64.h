#ifndef CODICIS_UTIL_BASE64_H
#define CODICIS_UTIL_BASE64_H

/**
 * @file base64.h
 * @brief Standard Base64 encoding (RFC 4648).
 */

#include <cstddef>
#include <cstdint>
#include <string>

namespace codicis {

/**
 * @brief Base64-encode a byte range.
 * @param data Pointer to the bytes.
 * @param len  Number of bytes.
 * @return The Base64 text (with '=' padding).
 */
std::string Base64Encode(const std::uint8_t* data, std::size_t len);

/**
 * @brief Base64-encode a string's bytes.
 * @param s The input bytes.
 * @return The Base64 text.
 */
std::string Base64Encode(const std::string& s);

}  // namespace codicis

#endif  // CODICIS_UTIL_BASE64_H
