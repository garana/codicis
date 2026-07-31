/**
 * @file option.cc
 * @brief Implementation of OptionRegistry (see option.h).
 */

#include "codicis/config/option.h"

#include <utility>

namespace codicis {

void OptionRegistry::add_string(std::string name, std::string default_value,
                                std::string help) {
  specs_.push_back(OptionSpec{std::move(name), OptionType::kString,
                              std::move(default_value), std::move(help)});
}

void OptionRegistry::add_bool(std::string name, bool default_value,
                              std::string help) {
  specs_.push_back(OptionSpec{std::move(name), OptionType::kBool,
                              default_value ? "true" : "false",
                              std::move(help)});
}

void OptionRegistry::add_int(std::string name, std::int64_t default_value,
                             std::string help) {
  specs_.push_back(OptionSpec{std::move(name), OptionType::kInt,
                              std::to_string(default_value),
                              std::move(help)});
}

void OptionRegistry::add_int_range(std::string name,
                                   std::int64_t default_value,
                                   std::int64_t min_value,
                                   std::int64_t max_value, std::string help) {
  OptionSpec spec{std::move(name), OptionType::kInt,
                  std::to_string(default_value), std::move(help)};
  spec.has_min = true;
  spec.min_value = min_value;
  spec.has_max = true;
  spec.max_value = max_value;
  specs_.push_back(std::move(spec));
}

const OptionSpec* OptionRegistry::find(std::string_view name) const {
  for (const OptionSpec& spec : specs_) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

}  // namespace codicis
