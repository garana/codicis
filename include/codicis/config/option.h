#ifndef CODICIS_CONFIG_OPTION_H
#define CODICIS_CONFIG_OPTION_H

/**
 * @file option.h
 * @brief Declarative registry of configuration options.
 *
 * Every configuration option is declared exactly once in an
 * @ref codicis::OptionRegistry. Both the file parser and the CLI parser
 * consume the same registry, which guarantees that a file key and its CLI
 * flag share an identical name (e.g. `net.http_port`). The registry also
 * carries defaults, types, and validation bounds.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace codicis {

/** @brief The value type of a configuration option. */
enum class OptionType {
  kString,  /**< An arbitrary UTF-8/ASCII string. */
  kInt,     /**< A signed 64-bit integer, optionally range-bounded. */
  kBool,    /**< A boolean (true/false/1/0/yes/no/on/off). */
};

/**
 * @brief Declaration of a single configuration option.
 */
struct OptionSpec {
  std::string name;           /**< Dotted name, e.g. "net.http_port". */
  OptionType type;            /**< Value type. */
  std::string default_value;  /**< Default, as an unparsed string. */
  std::string help;           /**< One-line description. */

  bool has_min = false;       /**< Whether @ref min_value applies (kInt). */
  std::int64_t min_value = 0; /**< Inclusive lower bound for kInt. */
  bool has_max = false;       /**< Whether @ref max_value applies (kInt). */
  std::int64_t max_value = 0; /**< Inclusive upper bound for kInt. */
};

/**
 * @brief An ordered collection of unique @ref OptionSpec declarations.
 */
class OptionRegistry {
 public:
  /**
   * @brief Register a string option.
   * @param name          Dotted option name.
   * @param default_value Default value.
   * @param help          One-line description.
   */
  void add_string(std::string name, std::string default_value,
                  std::string help);

  /**
   * @brief Register a boolean option.
   * @param name          Dotted option name.
   * @param default_value Default value ("true"/"false").
   * @param help          One-line description.
   */
  void add_bool(std::string name, bool default_value, std::string help);

  /**
   * @brief Register an integer option with no range bounds.
   * @param name          Dotted option name.
   * @param default_value Default value.
   * @param help          One-line description.
   */
  void add_int(std::string name, std::int64_t default_value,
               std::string help);

  /**
   * @brief Register an integer option constrained to [min, max].
   * @param name          Dotted option name.
   * @param default_value Default value (must lie within the range).
   * @param min_value     Inclusive lower bound.
   * @param max_value     Inclusive upper bound.
   * @param help          One-line description.
   */
  void add_int_range(std::string name, std::int64_t default_value,
                     std::int64_t min_value, std::int64_t max_value,
                     std::string help);

  /**
   * @brief Look up an option by name.
   * @param name The dotted option name.
   * @return Pointer to the spec, or nullptr if unregistered.
   */
  const OptionSpec* find(std::string_view name) const;

  /** @return All registered specs, in registration order. */
  const std::vector<OptionSpec>& specs() const { return specs_; }

 private:
  std::vector<OptionSpec> specs_;
};

}  // namespace codicis

#endif  // CODICIS_CONFIG_OPTION_H
