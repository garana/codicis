/**
 * @file logging.cc
 * @brief Implementation of the minimal leveled logger (see logging.h).
 */

#include "codicis/util/logging.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <string>

namespace codicis {
namespace {

/** @brief Process-wide threshold; defaults to info. */
LogLevel g_level = LogLevel::kInfo;

/**
 * @brief Short uppercase tag for a level, used in the emitted line.
 * @param level The level to name.
 * @return A stable, null-terminated tag.
 */
const char* LevelTag(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace: return "TRACE";
    case LogLevel::kDebug: return "DEBUG";
    case LogLevel::kInfo:  return "INFO";
    case LogLevel::kWarn:  return "WARN";
    case LogLevel::kError: return "ERROR";
  }
  return "?";
}

}  // namespace

void SetLogLevel(LogLevel level) { g_level = level; }

LogLevel GetLogLevel() { return g_level; }

bool ParseLogLevel(std::string_view name, LogLevel* out) {
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) {
    lower.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "trace") { *out = LogLevel::kTrace; return true; }
  if (lower == "debug") { *out = LogLevel::kDebug; return true; }
  if (lower == "info")  { *out = LogLevel::kInfo;  return true; }
  if (lower == "warn")  { *out = LogLevel::kWarn;  return true; }
  if (lower == "error") { *out = LogLevel::kError; return true; }
  return false;
}

void LogMessage(LogLevel level, std::string_view msg) {
  if (level < g_level) {
    return;
  }
  std::fprintf(stderr, "[%s] %.*s\n", LevelTag(level),
               static_cast<int>(msg.size()), msg.data());
}

}  // namespace codicis
