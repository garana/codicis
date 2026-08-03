/**
 * @file main.cc
 * @brief Reference auth helper: validates a credential to a user UUID.
 *
 * Reads @ref codicis::HelperMessage records from stdin (text codec) and writes
 * responses to stdout, demonstrating the auth protocol used by
 * @ref codicis::AuthClient. A `validate` request carries a `credential` field;
 * the response is either `status=ok` with a `user` UUID (and an optional
 * `not_after` absolute expiry in epoch nanoseconds) or `status=denied`.
 *
 * The reference rule is deliberately trivial and NOT secure: a credential is
 * "<user-uuid>:<secret>"; it is accepted only when the left part is a valid
 * UUID and the secret is exactly "good". A real helper would verify a signature
 * or look the token up in an identity store, and could run crypto -- which is
 * why the client runs a pool of these and caches results.
 */

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>

#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/helper_message.h"
#include "codicis/util/buffer.h"
#include "codicis/util/clock.h"
#include "codicis/util/uuid.h"

namespace {

using codicis::Buffer;
using codicis::HelperDecode;
using codicis::HelperMessage;
using codicis::IsValidUuidString;
using codicis::TextHelperCodec;
using codicis::WallClock;

/** @brief Write all bytes to @p fd, retrying short writes. */
void WriteAll(int fd, const std::string& data) {
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return;  // stdout closed; nothing more we can do
    }
  }
}

/** @brief One hour, in nanoseconds -- the reference credential lifetime. */
constexpr std::int64_t kOneHourNs = 3600LL * 1'000'000'000LL;

/**
 * @brief Produce the response for a request.
 * @param req The decoded request.
 * @return The response message.
 */
HelperMessage Handle(const HelperMessage& req) {
  HelperMessage resp;
  resp.req_id = req.req_id;
  if (req.type != "validate") {
    resp.type = "error";
    resp.set("reason", "unknown type: " + req.type);
    return resp;
  }
  resp.type = "validate";

  const std::string* cred = req.get("credential");
  const std::size_t colon = cred != nullptr ? cred->find(':') : std::string::npos;
  if (cred == nullptr || colon == std::string::npos) {
    resp.set("status", "denied");
    return resp;
  }
  const std::string user = cred->substr(0, colon);
  const std::string secret = cred->substr(colon + 1);
  if (IsValidUuidString(user) && secret == "good") {
    resp.set("status", "ok");
    resp.set("user", user);
    resp.set("not_after", std::to_string(WallClock().now() + kOneHourNs));
  } else {
    resp.set("status", "denied");
  }
  return resp;
}

}  // namespace

int main() {
  TextHelperCodec codec;
  Buffer in;

  for (;;) {
    // Drain all complete requests currently buffered.
    for (;;) {
      HelperMessage req;
      std::string err;
      const HelperDecode d = codec.decode(in, &req, &err);
      if (d == HelperDecode::kIncomplete) {
        break;
      }
      if (d == HelperDecode::kError) {
        return 1;
      }
      std::string out;
      codec.encode(Handle(req), &out);
      WriteAll(STDOUT_FILENO, out);
    }

    std::uint8_t* dst = in.reserve(4096);
    const ssize_t n = ::read(STDIN_FILENO, dst, 4096);
    if (n <= 0) {
      break;  // EOF or error
    }
    in.commit(static_cast<std::size_t>(n));
  }
  return 0;
}
