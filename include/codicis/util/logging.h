#ifndef CODICIS_UTIL_LOGGING_H
#define CODICIS_UTIL_LOGGING_H

/**
 * @file logging.h
 * @brief Minimal leveled logging to stderr.
 *
 * A tiny, dependency-free logger. The process-wide threshold is set once at
 * startup from configuration; messages below it are dropped cheaply. Helper
 * child processes use stderr for logs, keeping stdout/stdin for the protocol.
 */

#include <string_view>

namespace codicis {

/** @brief Severity levels, in increasing order of importance. */
enum class LogLevel {
  kTrace = 0,
  kDebug,
  kInfo,
  kWarn,
  kError,
};

/**
 * @brief Set the process-wide minimum level that will be emitted.
 * @param level Messages below this level are suppressed.
 */
void SetLogLevel(LogLevel level);

/** @return The current process-wide minimum log level. */
LogLevel GetLogLevel();

/**
 * @brief Parse a level name ("trace","debug","info","warn","error").
 * @param name Case-insensitive level name.
 * @param out  Receives the parsed level on success.
 * @return True if @p name was recognized.
 */
bool ParseLogLevel(std::string_view name, LogLevel* out);

/**
 * @brief Emit a pre-formatted message at @p level if it passes the threshold.
 * @param level The severity of the message.
 * @param msg   The fully formatted message (no trailing newline needed).
 */
void LogMessage(LogLevel level, std::string_view msg);

/** @return True if @p level would currently be emitted. */
inline bool LogEnabled(LogLevel level) { return level >= GetLogLevel(); }

}  // namespace codicis

#endif  // CODICIS_UTIL_LOGGING_H
