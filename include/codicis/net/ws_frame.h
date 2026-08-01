#ifndef CODICIS_NET_WS_FRAME_H
#define CODICIS_NET_WS_FRAME_H

/**
 * @file ws_frame.h
 * @brief RFC 6455 WebSocket frame encode/decode.
 *
 * Pure, allocation-simple codec with no protocol library. The decoder is
 * incremental: it consumes a whole frame from a @ref codicis::Buffer only once
 * fully present, unmasking the payload as required.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "codicis/util/buffer.h"

namespace codicis {

/** @brief WebSocket frame opcodes (RFC 6455 section 5.2). */
enum class WsOpcode : std::uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

/** @brief A decoded WebSocket frame with its (unmasked) payload. */
struct WsFrame {
  bool fin = true;
  WsOpcode opcode = WsOpcode::kText;
  std::string payload;

  /** @return True if this is a control frame (close/ping/pong). */
  bool is_control() const {
    return (static_cast<std::uint8_t>(opcode) & 0x08) != 0;
  }
};

/** @brief Outcome of @ref DecodeWsFrame. */
enum class WsDecode {
  kIncomplete,  /**< Need more bytes; nothing consumed. */
  kComplete,    /**< A frame was decoded and consumed. */
  kError,       /**< Protocol violation; connection should close. */
};

/**
 * @brief Decode one frame from @p in.
 * @param in           The receive buffer; consumed on kComplete.
 * @param out          Receives the decoded frame on kComplete.
 * @param err          Receives an error message on kError.
 * @param max_payload  Maximum accepted payload length.
 * @return The decode outcome.
 */
WsDecode DecodeWsFrame(Buffer& in, WsFrame* out, std::string* err,
                       std::size_t max_payload = 16 * 1024 * 1024);

/**
 * @brief Encode a frame.
 * @param opcode  The frame opcode.
 * @param payload The payload bytes.
 * @param fin     Whether this is the final fragment.
 * @param mask    Optional 4-byte masking key (client frames must be masked;
 *                server frames must not).
 * @return The encoded frame bytes.
 */
std::string EncodeWsFrame(
    WsOpcode opcode, std::string_view payload, bool fin = true,
    std::optional<std::array<std::uint8_t, 4>> mask = std::nullopt);

}  // namespace codicis

#endif  // CODICIS_NET_WS_FRAME_H
