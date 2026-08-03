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
#include <map>
#include <string>
#include <utility>

#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/helper_message.h"
#include "codicis/util/buffer.h"

namespace {

using codicis::Buffer;
using codicis::HelperDecode;
using codicis::HelperMessage;
using codicis::TextHelperCodec;

/** @brief Parse a decimal integer (optionally signed); 0 on non-digits. */
std::int64_t ParseInt(const std::string& s) {
  std::int64_t v = 0;
  std::size_t i = 0;
  bool neg = false;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    i = 1;
  }
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') {
      break;
    }
    v = v * 10 + (s[i] - '0');
  }
  return neg ? -v : v;
}

/** @brief What we remember about an order, to attribute its later fills. */
struct OrderInfo {
  std::string owner;   /**< Owning user UUID. */
  std::string side;    /**< "buy" or "sell". */
  std::string symbol;  /**< Instrument. */
};

/** @brief In-memory system of record for the reference helper. */
struct State {
  std::uint64_t highest = 0;  /**< Highest reported req_id (commit watermark). */
  std::map<std::uint64_t, OrderInfo> order_info;  /**< order id -> attribution. */
  std::map<std::pair<std::string, std::string>, std::int64_t>
      positions;  /**< (owner, symbol) -> signed net position. */
};

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
 * @brief Produce the response for a request, updating @p st.
 * @param st  The helper's in-memory state (watermark, orders, positions).
 * @param req The decoded request.
 * @return The response message.
 */
HelperMessage Handle(State* st, const HelperMessage& req) {
  HelperMessage resp;
  resp.req_id = req.req_id;
  if (req.type == "report_order") {
    st->highest = std::max(st->highest, req.req_id);
    // Remember the order so its later fills can be attributed to an account.
    if (const std::string* id = req.get("id")) {
      OrderInfo info;
      if (const std::string* o = req.get("owner")) info.owner = *o;
      if (const std::string* s = req.get("side")) info.side = *s;
      if (const std::string* sym = req.get("symbol")) info.symbol = *sym;
      st->order_info[static_cast<std::uint64_t>(ParseInt(*id))] = info;
    }
    resp.type = req.type;
    resp.set("status", "ok");
  } else if (req.type == "report_fill") {
    st->highest = std::max(st->highest, req.req_id);
    // Update the owning account's net position for the symbol (buy +, sell -).
    const std::string* id = req.get("id");
    const std::string* qty = req.get("qty");
    if (id != nullptr && qty != nullptr) {
      const auto it =
          st->order_info.find(static_cast<std::uint64_t>(ParseInt(*id)));
      if (it != st->order_info.end() && !it->second.owner.empty()) {
        const std::int64_t q = ParseInt(*qty);
        st->positions[{it->second.owner, it->second.symbol}] +=
            it->second.side == "buy" ? q : -q;
      }
    }
    resp.type = req.type;
    resp.set("status", "ok");
  } else if (req.type == "report_trade") {
    st->highest = std::max(st->highest, req.req_id);
    resp.type = req.type;
    resp.set("status", "ok");
  } else if (req.type == "commit") {
    resp.type = "commit";
    resp.set("committed", std::to_string(st->highest));
  } else if (req.type == "pull_position") {
    resp.type = "position";
    std::int64_t net = 0;
    const std::string* user = req.get("user");
    const std::string* sym = req.get("symbol");
    if (user != nullptr && sym != nullptr) {
      const auto it = st->positions.find({*user, *sym});
      if (it != st->positions.end()) {
        net = it->second;
      }
    }
    resp.set("net", std::to_string(net));
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
  State st;

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
      codec.encode(Handle(&st, req), &out);
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
