#ifndef CODICIS_IPC_HELPER_CODEC_H
#define CODICIS_IPC_HELPER_CODEC_H

/**
 * @file helper_codec.h
 * @brief Wire encodings for helper messages: text key=value and binary.
 *
 * Two interchangeable codecs implement @ref codicis::HelperCodec:
 *  - @ref codicis::TextHelperCodec: newline-delimited `key=value` records
 *    terminated by a blank line. Human-readable; values may not contain
 *    newlines.
 *  - @ref codicis::BinaryHelperCodec: length-prefixed frames with
 *    length-prefixed fields. Compact and binary-safe.
 * Both are decode-incremental so they can be driven from a socket/pipe buffer.
 */

#include <string>

#include "codicis/ipc/helper_message.h"
#include "codicis/util/buffer.h"

namespace codicis {

/** @brief Outcome of a codec decode attempt. */
enum class HelperDecode {
  kIncomplete,  /**< Need more bytes; nothing consumed. */
  kComplete,    /**< A message was decoded and consumed. */
  kError,       /**< Malformed input; the connection should close. */
};

/**
 * @brief Abstract encoder/decoder for @ref HelperMessage.
 */
class HelperCodec {
 public:
  virtual ~HelperCodec() = default;

  /**
   * @brief Encode @p msg, appending its bytes to @p out.
   * @param msg The message to encode.
   * @param out The destination byte string (appended to).
   */
  virtual void encode(const HelperMessage& msg, std::string* out) const = 0;

  /**
   * @brief Decode one message from @p in.
   * @param in  The receive buffer; consumed on kComplete.
   * @param out Receives the message on kComplete.
   * @param err Receives an error string on kError.
   * @return The decode outcome.
   */
  virtual HelperDecode decode(Buffer& in, HelperMessage* out,
                              std::string* err) const = 0;
};

/**
 * @brief Newline-delimited `key=value` codec (blank line terminates).
 */
class TextHelperCodec final : public HelperCodec {
 public:
  void encode(const HelperMessage& msg, std::string* out) const override;
  HelperDecode decode(Buffer& in, HelperMessage* out,
                      std::string* err) const override;
};

/**
 * @brief Length-prefixed binary codec (binary-safe field values).
 */
class BinaryHelperCodec final : public HelperCodec {
 public:
  void encode(const HelperMessage& msg, std::string* out) const override;
  HelperDecode decode(Buffer& in, HelperMessage* out,
                      std::string* err) const override;
};

}  // namespace codicis

#endif  // CODICIS_IPC_HELPER_CODEC_H
