#ifndef CODICIS_UTIL_RESULT_H
#define CODICIS_UTIL_RESULT_H

/**
 * @file result.h
 * @brief Lightweight error-or-value type used across codicis.
 *
 * codicis avoids exceptions on hot paths. Functions that can fail return a
 * @ref codicis::Result (value-or-error) or @ref codicis::Status (error-only),
 * both built on a small @ref codicis::Error carrying a code and a message.
 */

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace codicis {

/**
 * @brief Coarse category of a failure, for programmatic handling.
 */
enum class ErrorCode {
  kOk = 0,      /**< No error. Not used inside a populated Error. */
  kInvalidArg,  /**< A caller-supplied argument was invalid. */
  kNotFound,    /**< A requested entity does not exist. */
  kConflict,    /**< The request conflicts with current state. */
  kIo,          /**< An I/O or system-call failure occurred. */
  kProtocol,    /**< A wire-protocol violation was detected. */
  kExhausted,   /**< A bounded resource (queue, buffer) is full. */
  kInternal,    /**< An unexpected internal invariant was broken. */
};

/**
 * @brief A failure value: a machine-readable code plus a human message.
 */
struct Error {
  ErrorCode code = ErrorCode::kInternal;
  std::string message;

  /**
   * @brief Construct an Error.
   * @param c   The error category.
   * @param msg A human-readable description.
   */
  Error(ErrorCode c, std::string msg)
      : code(c), message(std::move(msg)) {}
};

/**
 * @brief Convenience factory for an Error.
 * @param code The error category.
 * @param msg  A human-readable description.
 * @return The constructed Error.
 */
inline Error MakeError(ErrorCode code, std::string msg) {
  return Error(code, std::move(msg));
}

/**
 * @brief Either a value of type @p T or an @ref Error.
 * @tparam T The success value type.
 */
template <typename T>
class Result {
 public:
  /**
   * @brief Construct a success result holding @p value.
   * @param value The success value (moved in).
   */
  Result(T value) : slot_(std::move(value)) {}

  /**
   * @brief Construct a failure result holding @p error.
   * @param error The error value (moved in).
   */
  Result(Error error) : slot_(std::move(error)) {}

  /** @return True if this holds a value, false if it holds an error. */
  bool ok() const { return std::holds_alternative<T>(slot_); }

  /** @return True if this holds a value. */
  explicit operator bool() const { return ok(); }

  /**
   * @brief Access the success value. Precondition: ok() is true.
   * @return Reference to the contained value.
   */
  T& value() { return std::get<T>(slot_); }

  /** @copydoc value() */
  const T& value() const { return std::get<T>(slot_); }

  /**
   * @brief Access the error. Precondition: ok() is false.
   * @return Reference to the contained error.
   */
  const Error& error() const { return std::get<Error>(slot_); }

 private:
  std::variant<T, Error> slot_;
};

/**
 * @brief An error-only result, equivalent to Result<void>.
 *
 * Holds either "success" or an @ref Error.
 */
class Status {
 public:
  /** @brief Construct a success status. */
  Status() = default;

  /**
   * @brief Construct a failure status holding @p error.
   * @param error The error value (moved in).
   */
  Status(Error error) : error_(std::move(error)) {}

  /** @return True on success, false if an error is held. */
  bool ok() const { return !error_.has_value(); }

  /** @return True on success. */
  explicit operator bool() const { return ok(); }

  /**
   * @brief Access the error. Precondition: ok() is false.
   * @return Reference to the contained error.
   */
  const Error& error() const { return *error_; }

  /** @brief A shared success value. */
  static Status Ok() { return Status(); }

 private:
  std::optional<Error> error_;
};

}  // namespace codicis

#endif  // CODICIS_UTIL_RESULT_H
