#ifndef CODICIS_IPC_HELPER_MESSAGE_H
#define CODICIS_IPC_HELPER_MESSAGE_H

/**
 * @file helper_message.h
 * @brief The wire-agnostic message exchanged with child-process helpers.
 *
 * A @ref codicis::HelperMessage is a correlation id, a type name, and an
 * ordered set of string key/value fields. The same message is serialized by
 * either the text or binary @ref codicis::HelperCodec, so helper logic is
 * independent of the chosen encoding.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codicis {

/**
 * @brief A request or response exchanged with a helper.
 */
struct HelperMessage {
  std::uint64_t req_id = 0;  /**< Correlation id (echoed in the response). */
  std::string type;          /**< Message type, e.g. "report_order". */
  std::vector<std::pair<std::string, std::string>> fields;

  /**
   * @brief Look up a field value by key (first match).
   * @param key The field name.
   * @return Pointer to the value, or nullptr if absent.
   */
  const std::string* get(std::string_view key) const {
    for (const auto& kv : fields) {
      if (kv.first == key) {
        return &kv.second;
      }
    }
    return nullptr;
  }

  /**
   * @brief Append a field.
   * @param key   The field name.
   * @param value The field value.
   */
  void set(std::string key, std::string value) {
    fields.emplace_back(std::move(key), std::move(value));
  }
};

}  // namespace codicis

#endif  // CODICIS_IPC_HELPER_MESSAGE_H
