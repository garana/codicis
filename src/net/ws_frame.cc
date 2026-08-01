/**
 * @file ws_frame.cc
 * @brief Implementation of the WebSocket frame codec (see ws_frame.h).
 */

#include "codicis/net/ws_frame.h"

namespace codicis {
namespace {

/** @brief True if @p op is a defined opcode. */
bool IsValidOpcode(std::uint8_t op) {
  switch (op) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x8:
    case 0x9:
    case 0xA:
      return true;
    default:
      return false;
  }
}

}  // namespace

WsDecode DecodeWsFrame(Buffer& in, WsFrame* out, std::string* err,
                       std::size_t max_payload) {
  std::string_view v = in.view();
  if (v.size() < 2) {
    return WsDecode::kIncomplete;
  }
  const auto b0 = static_cast<std::uint8_t>(v[0]);
  const auto b1 = static_cast<std::uint8_t>(v[1]);

  const bool fin = (b0 & 0x80) != 0;
  const std::uint8_t opcode = b0 & 0x0F;
  const bool masked = (b1 & 0x80) != 0;
  std::uint64_t payload_len = b1 & 0x7F;

  std::size_t header_len = 2;
  if (payload_len == 126) {
    if (v.size() < 4) {
      return WsDecode::kIncomplete;
    }
    payload_len = (static_cast<std::uint64_t>(
                       static_cast<std::uint8_t>(v[2]))
                   << 8) |
                  static_cast<std::uint8_t>(v[3]);
    header_len = 4;
  } else if (payload_len == 127) {
    if (v.size() < 10) {
      return WsDecode::kIncomplete;
    }
    payload_len = 0;
    for (int i = 0; i < 8; ++i) {
      payload_len = (payload_len << 8) |
                    static_cast<std::uint8_t>(v[2 + static_cast<std::size_t>(i)]);
    }
    header_len = 10;
  }

  std::array<std::uint8_t, 4> mask_key{};
  if (masked) {
    if (v.size() < header_len + 4) {
      return WsDecode::kIncomplete;
    }
    for (std::size_t i = 0; i < 4; ++i) {
      mask_key[i] = static_cast<std::uint8_t>(v[header_len + i]);
    }
    header_len += 4;
  }

  if (!IsValidOpcode(opcode)) {
    *err = "invalid opcode";
    return WsDecode::kError;
  }
  const bool control = (opcode & 0x08) != 0;
  if (control && (!fin || payload_len > 125)) {
    *err = "invalid control frame";
    return WsDecode::kError;
  }
  if (payload_len > max_payload) {
    *err = "frame payload too large";
    return WsDecode::kError;
  }

  const std::size_t total =
      header_len + static_cast<std::size_t>(payload_len);
  if (v.size() < total) {
    return WsDecode::kIncomplete;
  }

  out->fin = fin;
  out->opcode = static_cast<WsOpcode>(opcode);
  out->payload.assign(v.data() + header_len,
                      static_cast<std::size_t>(payload_len));
  if (masked) {
    for (std::size_t i = 0; i < out->payload.size(); ++i) {
      out->payload[i] = static_cast<char>(
          static_cast<std::uint8_t>(out->payload[i]) ^ mask_key[i % 4]);
    }
  }
  in.consume(total);
  return WsDecode::kComplete;
}

std::string EncodeWsFrame(WsOpcode opcode, std::string_view payload, bool fin,
                          std::optional<std::array<std::uint8_t, 4>> mask) {
  std::string out;
  const std::uint8_t b0 = static_cast<std::uint8_t>(
      (fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(opcode));
  out.push_back(static_cast<char>(b0));

  const std::size_t len = payload.size();
  const std::uint8_t mask_bit = mask.has_value() ? 0x80 : 0x00;
  if (len < 126) {
    out.push_back(static_cast<char>(mask_bit |
                                    static_cast<std::uint8_t>(len)));
  } else if (len <= 0xFFFF) {
    out.push_back(static_cast<char>(mask_bit | 126));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
  } else {
    out.push_back(static_cast<char>(mask_bit | 127));
    for (int i = 7; i >= 0; --i) {
      out.push_back(static_cast<char>(
          (static_cast<std::uint64_t>(len) >> (8 * i)) & 0xFF));
    }
  }

  if (mask.has_value()) {
    const std::array<std::uint8_t, 4>& key = *mask;
    for (std::uint8_t k : key) {
      out.push_back(static_cast<char>(k));
    }
    out.reserve(out.size() + len);
    for (std::size_t i = 0; i < len; ++i) {
      out.push_back(static_cast<char>(
          static_cast<std::uint8_t>(payload[i]) ^ key[i % 4]));
    }
  } else {
    out.append(payload);
  }
  return out;
}

}  // namespace codicis
