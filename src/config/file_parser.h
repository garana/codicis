#ifndef CODICIS_CONFIG_FILE_PARSER_H
#define CODICIS_CONFIG_FILE_PARSER_H

/**
 * @file file_parser.h
 * @brief Parse a `key = value` configuration file into a raw string map.
 *
 * Internal to the config subsystem. Syntax:
 *   - One `key = value` assignment per line.
 *   - Blank lines and lines whose first non-space character is '#' are
 *     ignored.
 *   - Keys and values are trimmed of surrounding whitespace.
 * No type checking or registry validation happens here.
 */

#include <string>
#include <string_view>
#include <unordered_map>

#include "codicis/util/result.h"

namespace codicis {
namespace config_internal {

/**
 * @brief Parse the contents of a config file.
 * @param contents The full file text.
 * @param out      Receives parsed key/value pairs (later keys overwrite).
 * @return Ok, or an Error (with line number) on a malformed line.
 */
Status ParseConfigText(std::string_view contents,
                       std::unordered_map<std::string, std::string>* out);

/**
 * @brief Read and parse a config file from disk.
 * @param path The file path.
 * @param out  Receives parsed key/value pairs.
 * @return Ok, or an Error if the file cannot be read or is malformed.
 */
Status ParseConfigFile(std::string_view path,
                       std::unordered_map<std::string, std::string>* out);

}  // namespace config_internal
}  // namespace codicis

#endif  // CODICIS_CONFIG_FILE_PARSER_H
