/**
 * @file cli_parser.cc
 * @brief Implementation of the CLI flag parser (see cli_parser.h).
 */

#include "cli_parser.h"

#include <string>
#include <string_view>

namespace codicis {
namespace config_internal {
namespace {

/**
 * @brief Build an "unknown flag" style error.
 * @param what Prefix describing the problem.
 * @param name The offending flag name.
 * @return An Error with code kInvalidArg.
 */
Error FlagError(std::string_view what, std::string_view name) {
  return MakeError(ErrorCode::kInvalidArg,
                   std::string(what) + ": --" + std::string(name));
}

}  // namespace

Status ParseCli(const OptionRegistry& registry, int argc,
                const char* const* argv, CliParse* out) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg.rfind("--", 0) != 0) {
      return Status(MakeError(ErrorCode::kInvalidArg,
                              "unexpected argument: " + std::string(arg)));
    }
    arg.remove_prefix(2);
    if (arg.empty()) {
      return Status(MakeError(ErrorCode::kInvalidArg, "empty flag: --"));
    }

    // Split off an inline value if present.
    std::string_view name = arg;
    std::optional<std::string_view> inline_value;
    if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
      name = arg.substr(0, eq);
      inline_value = arg.substr(eq + 1);
    }

    // The special --config flag names the config file.
    if (name == "config") {
      if (inline_value) {
        out->config_path = std::string(*inline_value);
      } else if (i + 1 < argc) {
        out->config_path = std::string(argv[++i]);
      } else {
        return Status(FlagError("missing value for", name));
      }
      continue;
    }

    const OptionSpec* spec = registry.find(name);
    if (spec == nullptr) {
      return Status(FlagError("unknown option", name));
    }

    std::string value;
    if (inline_value) {
      value = std::string(*inline_value);
    } else if (spec->type == OptionType::kBool) {
      value = "true";  // bare boolean flag
    } else {
      // Consume the next token as the value, unless it looks like a flag.
      if (i + 1 < argc &&
          std::string_view(argv[i + 1]).rfind("--", 0) != 0) {
        value = argv[++i];
      } else {
        return Status(FlagError("missing value for", name));
      }
    }
    out->values[std::string(name)] = std::move(value);
  }
  return Status::Ok();
}

}  // namespace config_internal
}  // namespace codicis
