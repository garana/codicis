#ifndef CODICIS_NET_HTTP_PARSER_H
#define CODICIS_NET_HTTP_PARSER_H

/**
 * @file http_parser.h
 * @brief Incremental, hand-rolled HTTP/1.1 request parser.
 *
 * The parser consumes bytes from a @ref codicis::Buffer as they arrive from a
 * socket. It handles partial input across calls: the header block is parsed
 * atomically once its terminating CRLFCRLF has arrived, then the body is read
 * per Content-Length or chunked transfer-coding. Parsed bytes are consumed
 * from the buffer so any pipelined request that follows remains for the next
 * @ref parse. No dynamic protocol library is used.
 */

#include <cstddef>
#include <string>

#include "codicis/net/http_message.h"
#include "codicis/util/buffer.h"

namespace codicis {

/**
 * @brief Parses one HTTP request at a time from a byte buffer.
 */
class HttpRequestParser {
 public:
  /** @brief Outcome of a @ref parse call. */
  enum class Progress {
    kIncomplete,  /**< Need more bytes; call again after reading more. */
    kComplete,    /**< A full request is available via @ref request(). */
    kError,       /**< The input is malformed; see @ref error(). */
  };

  /**
   * @brief Construct a parser with optional size limits.
   * @param max_header_bytes Maximum size of the request line + headers block.
   * @param max_body_bytes   Maximum allowed message body size.
   */
  explicit HttpRequestParser(std::size_t max_header_bytes = 64 * 1024,
                             std::size_t max_body_bytes = 8 * 1024 * 1024)
      : max_header_bytes_(max_header_bytes),
        max_body_bytes_(max_body_bytes) {}

  /**
   * @brief Parse as much as possible from @p in.
   *
   * On kComplete, the bytes of the parsed request have been consumed from
   * @p in and @ref request() holds the result. On kError the buffer state is
   * unspecified and the connection should be closed.
   * @param in The receive buffer; consumed in place.
   * @return The parse progress.
   */
  Progress parse(Buffer& in);

  /** @return The parsed request (valid after kComplete). */
  const HttpRequest& request() const { return request_; }

  /** @return A description of the failure (valid after kError). */
  const std::string& error() const { return error_; }

  /** @brief Reset to parse the next (pipelined) request on the connection. */
  void reset();

 private:
  /** @brief High-level state across calls. */
  enum class State {
    kHead,        /**< Awaiting the full header block. */
    kBodyLength,  /**< Reading a Content-Length-delimited body. */
    kBodyChunked, /**< Reading a chunked body. */
    kComplete,
    kError,
  };

  /** @brief Phase within a chunked-transfer body. */
  enum class ChunkPhase {
    kSize,     /**< Reading the hex chunk-size line. */
    kData,     /**< Reading chunk data bytes. */
    kDataCrlf, /**< Consuming the CRLF after a chunk's data. */
    kTrailer,  /**< Reading optional trailers up to the final empty line. */
  };

  /**
   * @brief Parse the request line and headers from @p block.
   * @param block The header block, excluding the terminating CRLFCRLF.
   * @return True on success; on failure sets @ref error_.
   */
  bool parse_head(std::string_view block);

  /**
   * @brief Advance a chunked-transfer body from @p in.
   * @param in The receive buffer; consumed in place.
   * @return kIncomplete, kComplete, or kError.
   */
  Progress parse_chunked(Buffer& in);

  /** @brief Set the error state with a message and return kError. */
  Progress fail(std::string msg);

  std::size_t max_header_bytes_;
  std::size_t max_body_bytes_;
  State state_ = State::kHead;
  HttpRequest request_;
  std::string error_;

  std::size_t body_remaining_ = 0;  /**< Bytes left for kBodyLength. */
  ChunkPhase chunk_phase_ = ChunkPhase::kSize;
  std::size_t chunk_remaining_ = 0;  /**< Data bytes left in current chunk. */
  std::size_t body_total_ = 0;       /**< Accumulated body size (for cap). */
};

}  // namespace codicis

#endif  // CODICIS_NET_HTTP_PARSER_H
