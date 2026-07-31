#ifndef CODICIS_CONFIG_PARSE_UTIL_H
#define CODICIS_CONFIG_PARSE_UTIL_H

/**
 * @file parse_util.h
 * @brief Internal string helpers shared by the config parsers.
 *
 * Not part of the public interface; lives under src/config.
 */

#include <charconv>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace codicis {
namespace config_internal {

/**
 * @brief Remove leading and trailing ASCII whitespace.
 * @param s The input view.
 * @return A subview of @p s with surrounding whitespace removed.
 */
inline std::string_view Trim(std::string_view s) {
  std::size_t begin = 0;
  std::size_t end = s.size();
  while (begin < end &&
         std::isspace(static_cast<unsigned char>(s[begin]))) {
    ++begin;
  }
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(begin, end - begin);
}

/**
 * @brief Parse a boolean literal.
 *
 * Accepts (case-insensitive): true/false, 1/0, yes/no, on/off.
 * @param s   The input.
 * @param out Receives the parsed value on success.
 * @return True if @p s is a recognized boolean literal.
 */
inline bool ParseBool(std::string_view s, bool* out) {
  std::string lower;
  lower.reserve(s.size());
  for (char c : s) {
    lower.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
    *out = true;
    return true;
  }
  if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
    *out = false;
    return true;
  }
  return false;
}

/**
 * @brief Parse a signed 64-bit integer (base 10, optional leading '-').
 * @param s   The input.
 * @param out Receives the parsed value on success.
 * @return True if @p s is a valid integer consuming the whole view.
 */
inline bool ParseInt(std::string_view s, std::int64_t* out) {
  if (s.empty()) {
    return false;
  }
  std::int64_t value = 0;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const auto res = std::from_chars(first, last, value);
  if (res.ec != std::errc() || res.ptr != last) {
    return false;
  }
  *out = value;
  return true;
}

}  // namespace config_internal
}  // namespace codicis

#endif  // CODICIS_CONFIG_PARSE_UTIL_H
