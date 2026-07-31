#ifndef CODICIS_CONFIG_CLI_PARSER_H
#define CODICIS_CONFIG_CLI_PARSER_H

/**
 * @file cli_parser.h
 * @brief Parse CLI flags into a raw string map (see config.h for semantics).
 *
 * Internal to the config subsystem. Recognized forms:
 *   --name=value      inline value
 *   --name value      separated value (value must not start with "--")
 *   --name            bare boolean flag (only for kBool options -> "true")
 *   --config <path>   special: names the config file, returned separately
 * Flags are resolved against the registry so bare booleans and unknown flags
 * are handled correctly; an unknown flag is an error.
 */

#include <optional>
#include <string>
#include <unordered_map>

#include "codicis/config/option.h"
#include "codicis/util/result.h"

namespace codicis {
namespace config_internal {

/** @brief Output of @ref ParseCli. */
struct CliParse {
  std::unordered_map<std::string, std::string> values;  /**< name -> value. */
  std::optional<std::string> config_path;  /**< From --config, if given. */
};

/**
 * @brief Parse argv into overrides plus an optional config-file path.
 * @param registry Declared options, used to resolve flag forms.
 * @param argc     Argument count (argv[0] is the program name).
 * @param argv     Argument vector.
 * @param out      Receives the parsed result on success.
 * @return Ok, or an Error describing the first malformed/unknown flag.
 */
Status ParseCli(const OptionRegistry& registry, int argc,
                const char* const* argv, CliParse* out);

}  // namespace config_internal
}  // namespace codicis

#endif  // CODICIS_CONFIG_CLI_PARSER_H
