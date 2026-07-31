/**
 * @file config.cc
 * @brief Implementation of Config load/merge/validate and typed getters.
 */

#include "codicis/config/config.h"

#include <sstream>
#include <string>
#include <utility>

#include "cli_parser.h"
#include "file_parser.h"
#include "parse_util.h"

namespace codicis {
namespace {

using config_internal::ParseBool;
using config_internal::ParseInt;

/**
 * @brief Validate a single value string against its option spec.
 * @param spec  The option declaration.
 * @param value The resolved value string.
 * @return Ok, or an Error describing the type/range violation.
 */
Status ValidateValue(const OptionSpec& spec, const std::string& value) {
  switch (spec.type) {
    case OptionType::kString:
      return Status::Ok();
    case OptionType::kBool: {
      bool b = false;
      if (!ParseBool(value, &b)) {
        return Status(MakeError(
            ErrorCode::kInvalidArg,
            "option '" + spec.name + "': not a boolean: '" + value + "'"));
      }
      return Status::Ok();
    }
    case OptionType::kInt: {
      std::int64_t n = 0;
      if (!ParseInt(value, &n)) {
        return Status(MakeError(
            ErrorCode::kInvalidArg,
            "option '" + spec.name + "': not an integer: '" + value + "'"));
      }
      if (spec.has_min && n < spec.min_value) {
        std::ostringstream m;
        m << "option '" << spec.name << "': " << n << " < min "
          << spec.min_value;
        return Status(MakeError(ErrorCode::kInvalidArg, m.str()));
      }
      if (spec.has_max && n > spec.max_value) {
        std::ostringstream m;
        m << "option '" << spec.name << "': " << n << " > max "
          << spec.max_value;
        return Status(MakeError(ErrorCode::kInvalidArg, m.str()));
      }
      return Status::Ok();
    }
  }
  return Status(MakeError(ErrorCode::kInternal, "unknown option type"));
}

}  // namespace

Result<Config> Config::load(const OptionRegistry& registry, int argc,
                            const char* const* argv) {
  return load_with_file(registry, std::string_view{}, argc, argv);
}

Result<Config> Config::load_with_file(const OptionRegistry& registry,
                                      std::string_view file_path, int argc,
                                      const char* const* argv) {
  // 1. Parse CLI first so --config can redirect the file path.
  config_internal::CliParse cli;
  if (Status s = config_internal::ParseCli(registry, argc, argv, &cli);
      !s.ok()) {
    return s.error();
  }

  std::string effective_file =
      cli.config_path ? *cli.config_path : std::string(file_path);

  // 2. Seed with registry defaults.
  std::unordered_map<std::string, std::string> values;
  for (const OptionSpec& spec : registry.specs()) {
    values[spec.name] = spec.default_value;
  }

  // 3. Overlay the config file, if any.
  if (!effective_file.empty()) {
    std::unordered_map<std::string, std::string> file_values;
    if (Status s =
            config_internal::ParseConfigFile(effective_file, &file_values);
        !s.ok()) {
      return s.error();
    }
    for (const auto& [key, value] : file_values) {
      if (registry.find(key) == nullptr) {
        return MakeError(ErrorCode::kInvalidArg,
                         "unknown option in config file: '" + key + "'");
      }
      values[key] = value;
    }
  }

  // 4. Overlay CLI flags (highest precedence). Names were checked by ParseCli.
  for (const auto& [key, value] : cli.values) {
    values[key] = value;
  }

  // 5. Validate every resolved value.
  for (const OptionSpec& spec : registry.specs()) {
    if (Status s = ValidateValue(spec, values[spec.name]); !s.ok()) {
      return s.error();
    }
  }

  return Config(registry, std::move(values));
}

Result<std::string> Config::get_raw(std::string_view name) const {
  if (registry_->find(name) == nullptr) {
    return MakeError(ErrorCode::kNotFound,
                     "unknown option: '" + std::string(name) + "'");
  }
  const auto it = values_.find(std::string(name));
  return it->second;
}

Result<std::string> Config::get_string(std::string_view name) const {
  return get_raw(name);
}

Result<std::int64_t> Config::get_int(std::string_view name) const {
  Result<std::string> raw = get_raw(name);
  if (!raw.ok()) {
    return raw.error();
  }
  std::int64_t n = 0;
  if (!ParseInt(raw.value(), &n)) {
    return MakeError(ErrorCode::kInvalidArg,
                     "option '" + std::string(name) + "' is not an integer");
  }
  return n;
}

Result<bool> Config::get_bool(std::string_view name) const {
  Result<std::string> raw = get_raw(name);
  if (!raw.ok()) {
    return raw.error();
  }
  bool b = false;
  if (!ParseBool(raw.value(), &b)) {
    return MakeError(ErrorCode::kInvalidArg,
                     "option '" + std::string(name) + "' is not a boolean");
  }
  return b;
}

}  // namespace codicis
