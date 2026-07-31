#ifndef CODICIS_CONFIG_CONFIG_H
#define CODICIS_CONFIG_CONFIG_H

/**
 * @file config.h
 * @brief Resolved configuration, merged from defaults, a file, and CLI flags.
 *
 * @ref codicis::Config::load resolves values in increasing precedence:
 * registry defaults, then the config file (if any), then CLI flags. CLI flags
 * therefore win, and they share names with file keys via the shared
 * @ref codicis::OptionRegistry. The resulting Config is immutable; subsystems
 * read typed values through the getters.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "codicis/config/option.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief Immutable, validated configuration values.
 */
class Config {
 public:
  /**
   * @brief Load configuration from CLI arguments and an optional file.
   *
   * Resolution order (later overrides earlier):
   *   1. Defaults from @p registry.
   *   2. The file named by `--config <path>` / `--config=<path>`, if present.
   *   3. Remaining CLI flags (`--name value` or `--name=value`).
   *
   * All values are validated against @p registry (type, range); unknown keys
   * in the file or on the CLI are errors.
   *
   * @param registry The declared options.
   * @param argc     Argument count (argv[0] is the program name).
   * @param argv     Argument vector.
   * @return The resolved Config, or an Error describing the first problem.
   */
  static Result<Config> load(const OptionRegistry& registry, int argc,
                             const char* const* argv);

  /**
   * @brief Load configuration from a file path plus CLI arguments.
   *
   * Like @ref load, but the file path is supplied directly (a `--config`
   * flag, if also present, overrides @p file_path). An empty @p file_path
   * means "no file".
   *
   * @param registry  The declared options.
   * @param file_path Path to a config file, or empty for none.
   * @param argc      Argument count.
   * @param argv      Argument vector.
   * @return The resolved Config, or an Error.
   */
  static Result<Config> load_with_file(const OptionRegistry& registry,
                                       std::string_view file_path, int argc,
                                       const char* const* argv);

  /**
   * @brief Get a string option value.
   * @param name The dotted option name (must be registered).
   * @return The value, or an Error if the option is unknown.
   */
  Result<std::string> get_string(std::string_view name) const;

  /**
   * @brief Get an integer option value.
   * @param name The dotted option name (must be registered as kInt).
   * @return The value, or an Error if unknown or mistyped.
   */
  Result<std::int64_t> get_int(std::string_view name) const;

  /**
   * @brief Get a boolean option value.
   * @param name The dotted option name (must be registered as kBool).
   * @return The value, or an Error if unknown or mistyped.
   */
  Result<bool> get_bool(std::string_view name) const;

  /**
   * @brief Get the raw (string) form of any option value.
   * @param name The dotted option name (must be registered).
   * @return The raw value, or an Error if the option is unknown.
   */
  Result<std::string> get_raw(std::string_view name) const;

 private:
  Config(const OptionRegistry& registry,
         std::unordered_map<std::string, std::string> values)
      : registry_(&registry), values_(std::move(values)) {}

  const OptionRegistry* registry_;
  std::unordered_map<std::string, std::string> values_;
};

}  // namespace codicis

#endif  // CODICIS_CONFIG_CONFIG_H
