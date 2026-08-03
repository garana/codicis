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
#include <tuple>
#include <utility>
#include <vector>

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

/** @brief A deep (non-resident) resting order the engine can pull back. */
struct DeepOrder {
  std::uint64_t id = 0;
  std::int64_t price = 0;
  std::int64_t leaves = 0;
  std::uint64_t seq = 0;
};

/** @brief In-memory system of record for the reference helper. */
struct State {
  std::uint64_t highest = 0;  /**< Highest reported req_id (commit watermark). */
  std::map<std::uint64_t, OrderInfo> order_info;  /**< order id -> attribution. */
  std::map<std::pair<std::string, std::string>, std::int64_t>
      positions;  /**< (owner, symbol) -> signed net position. */
  // Deep resting orders, keyed by (symbol, side) -> price -> FIFO by seq.
  std::map<std::pair<std::string, std::string>,
           std::map<std::int64_t, std::vector<DeepOrder>>>
      deep;
  // id -> (symbol, side, price), to locate a deep order for removal.
  std::map<std::uint64_t, std::tuple<std::string, std::string, std::int64_t>>
      deep_index;
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
  } else if (req.type == "report_deep") {
    // Record a resting order that lives only in storage (deep / evicted).
    const std::string* sym = req.get("symbol");
    const std::string* side = req.get("side");
    const std::string* id = req.get("id");
    if (sym != nullptr && side != nullptr && id != nullptr) {
      DeepOrder o;
      o.id = static_cast<std::uint64_t>(ParseInt(*id));
      o.price = req.get("price") != nullptr ? ParseInt(*req.get("price")) : 0;
      o.leaves = req.get("leaves") != nullptr ? ParseInt(*req.get("leaves")) : 0;
      o.seq = req.get("seq") != nullptr
                  ? static_cast<std::uint64_t>(ParseInt(*req.get("seq")))
                  : 0;
      st->deep[{*sym, *side}][o.price].push_back(o);
      st->deep_index[o.id] = {*sym, *side, o.price};
    }
    resp.type = "report_deep";
    resp.set("status", "ok");
  } else if (req.type == "remove_deep") {
    const std::string* id = req.get("id");
    if (id != nullptr) {
      const std::uint64_t oid = static_cast<std::uint64_t>(ParseInt(*id));
      const auto xit = st->deep_index.find(oid);
      if (xit != st->deep_index.end()) {
        const auto& [s, side, price] = xit->second;
        const auto mit = st->deep.find({s, side});
        if (mit != st->deep.end()) {
          const auto pit = mit->second.find(price);
          if (pit != mit->second.end()) {
            auto& v = pit->second;
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [oid](const DeepOrder& d) {
                                     return d.id == oid;
                                   }),
                    v.end());
            if (v.empty()) {
              mit->second.erase(pit);
            }
            if (mit->second.empty()) {
              st->deep.erase(mit);
            }
          }
        }
        st->deep_index.erase(xit);
      }
    }
    resp.type = "remove_deep";
    resp.set("status", "ok");
  } else if (req.type == "pull_levels") {
    // Return the best deep levels beyond from_price, best-price first and, in
    // each level, in arrival (seq) order -- a multi-record "orders" blob of
    // "id,price,leaves,seq" tuples separated by ';'.
    resp.type = "levels";
    const std::string* sym = req.get("symbol");
    const std::string* side = req.get("side");
    const std::int64_t from_price =
        req.get("from_price") != nullptr ? ParseInt(*req.get("from_price")) : 0;
    const std::int64_t count =
        req.get("count") != nullptr ? ParseInt(*req.get("count")) : 0;
    std::string blob;
    std::int64_t levels = 0;
    auto emit = [&](std::vector<DeepOrder> orders) {
      std::sort(orders.begin(), orders.end(),
                [](const DeepOrder& a, const DeepOrder& b) {
                  return a.seq < b.seq;  // arrival order within the level
                });
      for (const DeepOrder& o : orders) {
        blob += std::to_string(o.id) + "," + std::to_string(o.price) + "," +
                std::to_string(o.leaves) + "," + std::to_string(o.seq) + ";";
      }
      ++levels;
    };
    if (sym != nullptr && side != nullptr) {
      const auto mit = st->deep.find({*sym, *side});
      if (mit != st->deep.end()) {
        const auto& prices = mit->second;  // ascending by price
        if (*side == "buy") {  // best deep bid = highest price below the window
          for (auto it = prices.rbegin(); it != prices.rend() && levels < count;
               ++it) {
            if (it->first < from_price) {
              emit(it->second);
            }
          }
        } else {  // best deep ask = lowest price above the window
          for (auto it = prices.begin(); it != prices.end() && levels < count;
               ++it) {
            if (it->first > from_price) {
              emit(it->second);
            }
          }
        }
      }
      resp.set("symbol", *sym);
      resp.set("side", *side);
    }
    resp.set("orders", blob);
    resp.set("count", std::to_string(levels));
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
