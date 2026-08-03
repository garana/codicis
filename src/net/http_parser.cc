/**
 * @file http_parser.cc
 * @brief Implementation of the incremental HTTP/1.1 request parser.
 */

#include "codicis/net/http_parser.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace codicis {
namespace {

constexpr std::string_view kCrlf = "\r\n";
constexpr std::string_view kCrlfCrlf = "\r\n\r\n";

/** @brief ASCII case-insensitive equality. */
bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

/** @brief True if @p c is an RFC 7230 token character (tchar). */
bool IsTokenChar(unsigned char c) {
  if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z')) {
    return true;
  }
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

/** @brief True if @p s is a non-empty RFC 7230 token (e.g. a header name). */
bool IsToken(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  for (const char c : s) {
    if (!IsTokenChar(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

/**
 * @brief True if @p s contains a control character (a field-injection risk).
 *
 * Control bytes 0x00-0x1F (except HT) and DEL (0x7F) must not appear in a
 * request-line or header field -- a bare CR/LF or NUL there is a header-
 * injection / smuggling vector.
 */
bool HasCtl(std::string_view s) {
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if ((u < 0x20 && u != '\t') || u == 0x7F) {
      return true;
    }
  }
  return false;
}

/** @brief Remove leading and trailing spaces and tabs. */
std::string_view TrimOws(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t')) {
    ++b;
  }
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) {
    --e;
  }
  return s.substr(b, e - b);
}

/**
 * @brief Parse a hex chunk-size token (stops at ';' or end).
 * @param s   The chunk-size line (without CRLF).
 * @param out Receives the parsed size.
 * @return True if at least one hex digit was parsed.
 */
bool ParseHexSize(std::string_view s, std::size_t* out) {
  std::size_t value = 0;
  std::size_t digits = 0;
  for (char c : s) {
    if (c == ';') {
      break;  // chunk extensions ignored
    }
    int d;
    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = 10 + (c - 'a');
    } else if (c >= 'A' && c <= 'F') {
      d = 10 + (c - 'A');
    } else {
      return false;
    }
    value = value * 16 + static_cast<std::size_t>(d);
    ++digits;
  }
  if (digits == 0) {
    return false;
  }
  *out = value;
  return true;
}

}  // namespace

void HttpRequestParser::reset() {
  state_ = State::kHead;
  request_ = HttpRequest{};
  error_.clear();
  body_remaining_ = 0;
  chunk_phase_ = ChunkPhase::kSize;
  chunk_remaining_ = 0;
  body_total_ = 0;
}

HttpRequestParser::Progress HttpRequestParser::fail(std::string msg) {
  state_ = State::kError;
  error_ = std::move(msg);
  return Progress::kError;
}

bool HttpRequestParser::parse_head(std::string_view block) {
  // Split into lines on CRLF; the first is the request line.
  const std::size_t rl_end = block.find(kCrlf);
  const std::string_view request_line =
      (rl_end == std::string_view::npos) ? block : block.substr(0, rl_end);

  // Request line: METHOD SP TARGET SP HTTP/1.x
  const std::size_t sp1 = request_line.find(' ');
  if (sp1 == std::string_view::npos) {
    error_ = "malformed request line";
    return false;
  }
  const std::size_t sp2 = request_line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos) {
    error_ = "malformed request line";
    return false;
  }
  request_.method = std::string(request_line.substr(0, sp1));
  request_.target =
      std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
  const std::string_view version = request_line.substr(sp2 + 1);
  if (version == "HTTP/1.0") {
    request_.http_minor = 0;
  } else if (version == "HTTP/1.1") {
    request_.http_minor = 1;
  } else {
    error_ = "unsupported HTTP version";
    return false;
  }
  if (request_.method.empty() || request_.target.empty()) {
    error_ = "empty method or target";
    return false;
  }
  // The method must be a token and the target must be free of control bytes
  // (a bare CR/LF/NUL there is an injection/smuggling vector).
  if (!IsToken(request_.method)) {
    error_ = "invalid method";
    return false;
  }
  if (HasCtl(request_.target)) {
    error_ = "control character in target";
    return false;
  }

  // Split target into path and query.
  const std::size_t q = request_.target.find('?');
  if (q == std::string::npos) {
    request_.path = request_.target;
  } else {
    request_.path = request_.target.substr(0, q);
    request_.query = request_.target.substr(q + 1);
  }

  // Headers.
  std::size_t pos = (rl_end == std::string_view::npos) ? block.size()
                                                       : rl_end + kCrlf.size();
  while (pos < block.size()) {
    std::size_t eol = block.find(kCrlf, pos);
    if (eol == std::string_view::npos) {
      eol = block.size();
    }
    std::string_view line = block.substr(pos, eol - pos);
    pos = (eol == block.size()) ? block.size() : eol + kCrlf.size();
    if (line.empty()) {
      continue;
    }
    // Reject obsolete line folding (a continuation line beginning with SP/HT):
    // it is deprecated by RFC 7230 and a request-smuggling desync vector.
    if (line[0] == ' ' || line[0] == '\t') {
      error_ = "obsolete header line folding";
      return false;
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      error_ = "malformed header line";
      return false;
    }
    // The name must be a token (no whitespace before the colon, no controls);
    // the value must contain no control characters.
    std::string_view name = line.substr(0, colon);
    std::string_view value = TrimOws(line.substr(colon + 1));
    if (!IsToken(name)) {
      error_ = "invalid header name";
      return false;
    }
    if (HasCtl(value)) {
      error_ = "control character in header value";
      return false;
    }
    request_.headers.push_back(
        HttpHeader{std::string(name), std::string(value)});
  }

  // Resolve keep-alive: HTTP/1.1 defaults to keep-alive; a Connection header
  // may override in either direction.
  request_.keep_alive = (request_.http_minor == 1);
  if (const std::string* conn = request_.header("Connection")) {
    if (IEquals(TrimOws(*conn), "close")) {
      request_.keep_alive = false;
    } else if (IEquals(TrimOws(*conn), "keep-alive")) {
      request_.keep_alive = true;
    }
  }

  // Determine body framing. Scan for Content-Length occurrences first so we can
  // reject request-smuggling framing (conflicting duplicates; CL with TE).
  const std::string* te = request_.header("Transfer-Encoding");
  const std::string* cl = nullptr;
  for (const HttpHeader& h : request_.headers) {
    if (!IEquals(h.name, "Content-Length")) {
      continue;
    }
    if (cl != nullptr && TrimOws(h.value) != TrimOws(*cl)) {
      error_ = "conflicting Content-Length";  // smuggling vector
      return false;
    }
    cl = &h.value;
  }
  // A message with both Content-Length and Transfer-Encoding is ambiguous
  // (the classic CL.TE / TE.CL smuggling pair) -- reject it.
  if (te != nullptr && cl != nullptr) {
    error_ = "Content-Length with Transfer-Encoding";
    return false;
  }

  if (te != nullptr) {
    if (IEquals(TrimOws(*te), "chunked")) {
      state_ = State::kBodyChunked;
      chunk_phase_ = ChunkPhase::kSize;
      return true;
    }
    error_ = "unsupported Transfer-Encoding";
    return false;
  }
  if (cl != nullptr) {
    constexpr std::size_t kMax = static_cast<std::size_t>(-1);
    std::size_t len = 0;
    for (char c : TrimOws(*cl)) {
      if (c < '0' || c > '9') {
        error_ = "invalid Content-Length";
        return false;
      }
      if (len > (kMax - 9) / 10) {  // guard the multiply/add against overflow
        error_ = "Content-Length overflow";
        return false;
      }
      len = len * 10 + static_cast<std::size_t>(c - '0');
    }
    if (len > max_body_bytes_) {
      error_ = "body exceeds limit";
      return false;
    }
    body_remaining_ = len;
    state_ = (len == 0) ? State::kComplete : State::kBodyLength;
    return true;
  }

  state_ = State::kComplete;
  return true;
}

HttpRequestParser::Progress HttpRequestParser::parse_chunked(Buffer& in) {
  for (;;) {
    switch (chunk_phase_) {
      case ChunkPhase::kSize: {
        std::string_view v = in.view();
        const std::size_t eol = v.find(kCrlf);
        if (eol == std::string_view::npos) {
          if (v.size() > 1024) {
            return fail("chunk-size line too long");
          }
          return Progress::kIncomplete;
        }
        std::size_t size = 0;
        if (!ParseHexSize(v.substr(0, eol), &size)) {
          return fail("invalid chunk size");
        }
        in.consume(eol + kCrlf.size());
        if (size == 0) {
          chunk_phase_ = ChunkPhase::kTrailer;
        } else {
          body_total_ += size;
          if (body_total_ > max_body_bytes_) {
            return fail("body exceeds limit");
          }
          chunk_remaining_ = size;
          chunk_phase_ = ChunkPhase::kData;
        }
        break;
      }
      case ChunkPhase::kData: {
        std::string_view v = in.view();
        const std::size_t take = std::min(v.size(), chunk_remaining_);
        request_.body.append(v.data(), take);
        in.consume(take);
        chunk_remaining_ -= take;
        if (chunk_remaining_ > 0) {
          return Progress::kIncomplete;
        }
        chunk_phase_ = ChunkPhase::kDataCrlf;
        break;
      }
      case ChunkPhase::kDataCrlf: {
        std::string_view v = in.view();
        if (v.size() < 2) {
          return Progress::kIncomplete;
        }
        if (v[0] != '\r' || v[1] != '\n') {
          return fail("missing CRLF after chunk data");
        }
        in.consume(2);
        chunk_phase_ = ChunkPhase::kSize;
        break;
      }
      case ChunkPhase::kTrailer: {
        std::string_view v = in.view();
        const std::size_t eol = v.find(kCrlf);
        if (eol == std::string_view::npos) {
          return Progress::kIncomplete;
        }
        in.consume(eol + kCrlf.size());
        if (eol == 0) {
          state_ = State::kComplete;  // final empty line
          return Progress::kComplete;
        }
        break;  // a trailer header line; keep reading
      }
    }
  }
}

HttpRequestParser::Progress HttpRequestParser::parse(Buffer& in) {
  for (;;) {
    switch (state_) {
      case State::kHead: {
        std::string_view v = in.view();
        const std::size_t term = v.find(kCrlfCrlf);
        if (term == std::string_view::npos) {
          if (v.size() > max_header_bytes_) {
            return fail("header block too large");
          }
          return Progress::kIncomplete;
        }
        if (term > max_header_bytes_) {
          return fail("header block too large");
        }
        const std::string_view block = v.substr(0, term);
        if (!parse_head(block)) {
          state_ = State::kError;
          return Progress::kError;
        }
        in.consume(term + kCrlfCrlf.size());
        break;  // re-loop into the new state
      }
      case State::kBodyLength: {
        std::string_view v = in.view();
        const std::size_t take = std::min(v.size(), body_remaining_);
        request_.body.append(v.data(), take);
        in.consume(take);
        body_remaining_ -= take;
        if (body_remaining_ > 0) {
          return Progress::kIncomplete;
        }
        state_ = State::kComplete;
        break;
      }
      case State::kBodyChunked: {
        const Progress p = parse_chunked(in);
        if (p != Progress::kComplete) {
          return p;
        }
        break;  // state_ set to kComplete inside parse_chunked
      }
      case State::kComplete:
        return Progress::kComplete;
      case State::kError:
        return Progress::kError;
    }
  }
}

}  // namespace codicis
