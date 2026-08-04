/**
 * @file main.cc
 * @brief Reference client-request ingress helper child process.
 *
 * An ingress helper is the *source* of client-side traffic: it pulls order and
 * cancel requests from some external system (Kafka, RabbitMQ, SQS, ...) and
 * pushes them into codicis over the standard @ref codicis::HelperCodec. Unlike
 * the storage/auth helpers, it *initiates*: it writes request records to its
 * stdout (codicis reads them) and reads reply records from its stdin.
 *
 * This reference stands in for the external source with a simple input file
 * (argv[1], or $CODICIS_INGRESS_INPUT), one request per line:
 *
 *     submit user=<uuid>&symbol=BTC&side=sell&type=limit&price=100&qty=10
 *     cancel user=<uuid>&order=<uuid>
 *
 * The leading token is the message @c type (submit/cancel); the rest is a
 * form-encoded body (the same fields the REST/WS order API accepts, plus a
 * per-request `user`). The body travels as a single `form` field so an order
 * field named `type` cannot collide with the codec's reserved envelope `type`.
 * It emits one record per line, then reads and logs replies to stderr until
 * every request has been answered or stdin closes.
 *
 * A production helper would replace the file source with its broker client and
 * keep the identical stdout/stdin protocol -- no codicis change required.
 */

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/helper_message.h"
#include "codicis/util/buffer.h"

namespace {

using codicis::Buffer;
using codicis::HelperDecode;
using codicis::HelperMessage;
using codicis::TextHelperCodec;

/** @brief Write all bytes to @p fd, retrying short writes and EINTR. */
bool WriteAll(int fd, const std::string& data) {
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n > 0) {
      off += static_cast<std::size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

/**
 * @brief Parse one input line "TYPE field=val&field=val" into a message.
 * @return True on a non-empty, non-comment line.
 */
bool ParseLine(const std::string& line, std::uint64_t req_id,
               HelperMessage* out) {
  std::size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  if (i >= line.size() || line[i] == '#') {
    return false;  // blank or comment
  }
  const std::size_t sp = line.find(' ', i);
  out->req_id = req_id;
  out->type = line.substr(i, sp == std::string::npos ? std::string::npos
                                                     : sp - i);
  out->fields.clear();
  // The whole remainder is the form body, carried as one `form` field so an
  // order field named `type` cannot collide with the envelope `type`.
  if (sp != std::string::npos) {
    out->set("form", line.substr(sp + 1));
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  TextHelperCodec codec;

  // The "external source": a file of request lines. A real helper swaps this
  // for its broker client without touching the stdout/stdin protocol below.
  std::string input_path;
  if (argc > 1) {
    input_path = argv[1];
  } else if (const char* env = std::getenv("CODICIS_INGRESS_INPUT")) {
    input_path = env;
  }

  std::uint64_t expected = 0;
  if (!input_path.empty()) {
    std::ifstream in(input_path);
    std::string line;
    std::uint64_t next_id = 1;
    while (std::getline(in, line)) {
      HelperMessage msg;
      if (!ParseLine(line, next_id, &msg)) {
        continue;
      }
      std::string bytes;
      codec.encode(msg, &bytes);
      if (!WriteAll(STDOUT_FILENO, bytes)) {
        return 1;
      }
      ++next_id;
      ++expected;
    }
  }

  // Read replies from codicis (our stdin) and log them until all requests are
  // answered or the pipe closes.
  Buffer buf;
  std::uint64_t got = 0;
  while (expected == 0 || got < expected) {
    for (;;) {
      HelperMessage reply;
      std::string err;
      const HelperDecode d = codec.decode(buf, &reply, &err);
      if (d == HelperDecode::kIncomplete) {
        break;
      }
      if (d == HelperDecode::kError) {
        return 1;
      }
      ++got;
      std::string log = "ingress reply req_id=" + std::to_string(reply.req_id);
      for (const auto& kv : reply.fields) {
        log += " " + kv.first + "=" + kv.second;
      }
      log += "\n";
      (void)WriteAll(STDERR_FILENO, log);
    }
    if (expected != 0 && got >= expected) {
      break;
    }
    char chunk[4096];
    const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (n > 0) {
      buf.append(std::string_view(chunk, static_cast<std::size_t>(n)));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;  // EOF (codicis closed the pipe) or a read error
    }
  }
  return 0;
}
