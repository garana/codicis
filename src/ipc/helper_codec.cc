/**
 * @file helper_codec.cc
 * @brief Implementation of the text and binary helper codecs.
 */

#include "codicis/ipc/helper_codec.h"

#include <cstring>

namespace codicis {
namespace {

constexpr std::uint32_t kBinaryMagic = 0x43'4F'44'43;  // "CODC"
constexpr std::size_t kMaxBinaryPayload = 64 * 1024 * 1024;

/** @brief Append a little-endian 16-bit value. */
void PutU16(std::string* out, std::uint16_t v) {
  out->push_back(static_cast<char>(v & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
}

/** @brief Append a little-endian 32-bit value. */
void PutU32(std::string* out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

/** @brief Append a little-endian 64-bit value. */
void PutU64(std::string* out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

/** @brief Read a little-endian unsigned integer of @p n bytes from @p p. */
std::uint64_t GetUint(const std::uint8_t* p, std::size_t n) {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

/** @brief Trim trailing '\r' for tolerance of CRLF line endings. */
std::string_view TrimCr(std::string_view s) {
  if (!s.empty() && s.back() == '\r') {
    s.remove_suffix(1);
  }
  return s;
}

}  // namespace

// ---- Text codec ------------------------------------------------------------

void TextHelperCodec::encode(const HelperMessage& msg,
                             std::string* out) const {
  out->append("req_id=");
  out->append(std::to_string(msg.req_id));
  out->push_back('\n');
  out->append("type=");
  out->append(msg.type);
  out->push_back('\n');
  for (const auto& kv : msg.fields) {
    out->append(kv.first);
    out->push_back('=');
    out->append(kv.second);
    out->push_back('\n');
  }
  out->push_back('\n');  // blank line terminates the record
}

HelperDecode TextHelperCodec::decode(Buffer& in, HelperMessage* out,
                                     std::string* err) const {
  const std::string_view v = in.view();
  const std::size_t term = v.find("\n\n");
  if (term == std::string_view::npos) {
    return HelperDecode::kIncomplete;
  }
  const std::string_view block = v.substr(0, term);

  HelperMessage msg;
  bool have_req_id = false;
  std::size_t pos = 0;
  while (pos <= block.size()) {
    std::size_t eol = block.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = block.size();
    }
    std::string_view line = TrimCr(block.substr(pos, eol - pos));
    pos = eol + 1;
    if (line.empty()) {
      continue;
    }
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      *err = "text codec: line without '='";
      return HelperDecode::kError;
    }
    const std::string_view key = line.substr(0, eq);
    const std::string_view value = line.substr(eq + 1);
    if (key == "req_id") {
      msg.req_id = 0;
      for (char c : value) {
        if (c < '0' || c > '9') {
          *err = "text codec: bad req_id";
          return HelperDecode::kError;
        }
        msg.req_id = msg.req_id * 10 + static_cast<std::uint64_t>(c - '0');
      }
      have_req_id = true;
    } else if (key == "type") {
      msg.type = std::string(value);
    } else {
      msg.set(std::string(key), std::string(value));
    }
  }
  if (!have_req_id) {
    *err = "text codec: missing req_id";
    return HelperDecode::kError;
  }

  in.consume(term + 2);
  *out = std::move(msg);
  return HelperDecode::kComplete;
}

// ---- Binary codec ----------------------------------------------------------

void BinaryHelperCodec::encode(const HelperMessage& msg,
                               std::string* out) const {
  std::string payload;
  PutU64(&payload, msg.req_id);
  PutU16(&payload, static_cast<std::uint16_t>(msg.type.size()));
  payload.append(msg.type);
  PutU32(&payload, static_cast<std::uint32_t>(msg.fields.size()));
  for (const auto& kv : msg.fields) {
    PutU32(&payload, static_cast<std::uint32_t>(kv.first.size()));
    payload.append(kv.first);
    PutU32(&payload, static_cast<std::uint32_t>(kv.second.size()));
    payload.append(kv.second);
  }
  PutU32(out, kBinaryMagic);
  PutU32(out, static_cast<std::uint32_t>(payload.size()));
  out->append(payload);
}

HelperDecode BinaryHelperCodec::decode(Buffer& in, HelperMessage* out,
                                       std::string* err) const {
  const std::string_view v = in.view();
  if (v.size() < 8) {
    return HelperDecode::kIncomplete;
  }
  const auto* base = reinterpret_cast<const std::uint8_t*>(v.data());
  const std::uint32_t magic = static_cast<std::uint32_t>(GetUint(base, 4));
  if (magic != kBinaryMagic) {
    *err = "binary codec: bad magic";
    return HelperDecode::kError;
  }
  const std::size_t payload_len =
      static_cast<std::size_t>(GetUint(base + 4, 4));
  if (payload_len > kMaxBinaryPayload) {
    *err = "binary codec: payload too large";
    return HelperDecode::kError;
  }
  if (v.size() < 8 + payload_len) {
    return HelperDecode::kIncomplete;
  }

  const std::uint8_t* p = base + 8;
  const std::uint8_t* end = p + payload_len;

  // Reads a length-prefixed field guarded against overruns.
  auto need = [&](std::size_t n) -> bool {
    return static_cast<std::size_t>(end - p) >= n;
  };

  if (!need(8 + 2)) {
    *err = "binary codec: truncated header";
    return HelperDecode::kError;
  }
  HelperMessage msg;
  msg.req_id = GetUint(p, 8);
  p += 8;
  const std::size_t type_len = static_cast<std::size_t>(GetUint(p, 2));
  p += 2;
  if (!need(type_len + 4)) {
    *err = "binary codec: truncated type";
    return HelperDecode::kError;
  }
  msg.type.assign(reinterpret_cast<const char*>(p), type_len);
  p += type_len;
  const std::size_t count = static_cast<std::size_t>(GetUint(p, 4));
  p += 4;

  for (std::size_t i = 0; i < count; ++i) {
    if (!need(4)) {
      *err = "binary codec: truncated field key length";
      return HelperDecode::kError;
    }
    const std::size_t klen = static_cast<std::size_t>(GetUint(p, 4));
    p += 4;
    if (!need(klen + 4)) {
      *err = "binary codec: truncated field key";
      return HelperDecode::kError;
    }
    std::string key(reinterpret_cast<const char*>(p), klen);
    p += klen;
    const std::size_t vlen = static_cast<std::size_t>(GetUint(p, 4));
    p += 4;
    if (!need(vlen)) {
      *err = "binary codec: truncated field value";
      return HelperDecode::kError;
    }
    std::string value(reinterpret_cast<const char*>(p), vlen);
    p += vlen;
    msg.set(std::move(key), std::move(value));
  }

  in.consume(8 + payload_len);
  *out = std::move(msg);
  return HelperDecode::kComplete;
}

}  // namespace codicis
