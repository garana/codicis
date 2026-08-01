/**
 * @file main.cc
 * @brief Reference storage helper: a child process persisting to memory.
 *
 * Reads @ref codicis::HelperMessage records from stdin (text codec), applies a
 * trivial in-memory persistence model, and writes responses to stdout. It
 * demonstrates the storage protocol used by @ref codicis::StorageClient:
 * report_order/report_trade acks, a commit watermark, and level pulls. A real
 * helper would durably persist and could be swapped in without engine changes.
 */

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <string>

#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/helper_message.h"
#include "codicis/util/buffer.h"

namespace {

using codicis::Buffer;
using codicis::HelperDecode;
using codicis::HelperMessage;
using codicis::TextHelperCodec;

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

/**
 * @brief Produce the response for a request.
 * @param req     The decoded request.
 * @param highest The highest reported req_id so far (updated here).
 * @return The response message.
 */
HelperMessage Handle(const HelperMessage& req, std::uint64_t* highest) {
  HelperMessage resp;
  resp.req_id = req.req_id;
  if (req.type == "report_order" || req.type == "report_trade" ||
      req.type == "report_fill") {
    *highest = std::max(*highest, req.req_id);
    resp.type = req.type;
    resp.set("status", "ok");
  } else if (req.type == "commit") {
    resp.type = "commit";
    resp.set("committed", std::to_string(*highest));
  } else if (req.type == "pull_levels") {
    resp.type = "levels";
    if (const std::string* s = req.get("symbol")) {
      resp.set("symbol", *s);
    }
    if (const std::string* s = req.get("side")) {
      resp.set("side", *s);
    }
    resp.set("count", "0");  // this reference helper stores no deep levels
  } else if (req.type == "ping") {
    resp.type = "pong";
  } else {
    resp.type = "error";
    resp.set("reason", "unknown type: " + req.type);
  }
  return resp;
}

}  // namespace

int main() {
  TextHelperCodec codec;
  Buffer in;
  std::uint64_t highest = 0;

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
      codec.encode(Handle(req, &highest), &out);
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
