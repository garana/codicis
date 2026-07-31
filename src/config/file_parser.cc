/**
 * @file file_parser.cc
 * @brief Implementation of the config file parser (see file_parser.h).
 */

#include "file_parser.h"

#include <fstream>
#include <sstream>
#include <string>

#include "parse_util.h"

namespace codicis {
namespace config_internal {

Status ParseConfigText(
    std::string_view contents,
    std::unordered_map<std::string, std::string>* out) {
  std::size_t line_no = 0;
  std::size_t pos = 0;
  while (pos <= contents.size()) {
    const std::size_t nl = contents.find('\n', pos);
    const std::size_t end =
        (nl == std::string_view::npos) ? contents.size() : nl;
    std::string_view line = contents.substr(pos, end - pos);
    ++line_no;

    std::string_view trimmed = Trim(line);
    if (!trimmed.empty() && trimmed.front() != '#') {
      const std::size_t eq = trimmed.find('=');
      if (eq == std::string_view::npos) {
        std::ostringstream msg;
        msg << "config line " << line_no << ": expected 'key = value'";
        return Status(MakeError(ErrorCode::kInvalidArg, msg.str()));
      }
      std::string_view key = Trim(trimmed.substr(0, eq));
      std::string_view value = Trim(trimmed.substr(eq + 1));
      if (key.empty()) {
        std::ostringstream msg;
        msg << "config line " << line_no << ": empty key";
        return Status(MakeError(ErrorCode::kInvalidArg, msg.str()));
      }
      (*out)[std::string(key)] = std::string(value);
    }

    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  return Status::Ok();
}

Status ParseConfigFile(
    std::string_view path,
    std::unordered_map<std::string, std::string>* out) {
  std::ifstream in{std::string(path), std::ios::binary};
  if (!in) {
    return Status(MakeError(ErrorCode::kIo,
                            "cannot open config file: " + std::string(path)));
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ParseConfigText(ss.str(), out);
}

}  // namespace config_internal
}  // namespace codicis
