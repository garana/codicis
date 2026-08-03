/**
 * @file test_engine.cc
 * @brief Tests for the report-before-place TradingEngine over a fake helper.
 */

#include "catch_amalgamated.hpp"

#include "codicis/core/order.h"
#include "codicis/engine/trading_engine.h"
#include "codicis/event/event_loop.h"
#include "codicis/ipc/helper_client.h"
#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/storage_client.h"
#include "codicis/util/clock.h"
#include "codicis/util/uuid.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

using namespace codicis;

namespace {

/** @brief Emulates the storage helper end of a socketpair. */
struct TestHelper {
  int fd;
  const HelperCodec& codec;
  Buffer in;

  /** @brief Drain and decode all buffered requests. */
  std::vector<HelperMessage> read_requests() {
    for (;;) {
      std::uint8_t buf[4096];
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0) {
        in.append(buf, static_cast<std::size_t>(n));
      } else {
        break;
      }
    }
    std::vector<HelperMessage> out;
    for (;;) {
      HelperMessage m;
      std::string err;
      if (codec.decode(in, &m, &err) != HelperDecode::kComplete) {
        break;
      }
      out.push_back(std::move(m));
    }
    return out;
  }

  /** @brief Write an encoded response to the client end. */
  void write_msg(const HelperMessage& resp) {
    std::string bytes;
    codec.encode(resp, &bytes);
    std::size_t off = 0;
    while (off < bytes.size()) {
      const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
      if (n > 0) {
        off += static_cast<std::size_t>(n);
      } else {
        break;
      }
    }
  }

  /** @brief Acknowledge a request by echoing its id with status ok. */
  void ack(std::uint64_t req_id, const std::string& type) {
    HelperMessage resp;
    resp.req_id = req_id;
    resp.type = type;
    resp.set("status", "ok");
    write_msg(resp);
  }

  /** @brief Answer a pull_position request with a net position. */
  void reply_position(std::uint64_t req_id, std::int64_t net) {
    HelperMessage resp;
    resp.req_id = req_id;
    resp.type = "position";
    resp.set("net", std::to_string(net));
    write_msg(resp);
  }

  /**
   * @brief Ack every buffered report_order and answer every pull_position.
   * @param net The net position to report for any pull_position.
   */
  void serve(std::int64_t net) {
    for (const HelperMessage& m : read_requests()) {
      if (m.type == "report_order") {
        ack(m.req_id, "report_order");
      } else if (m.type == "pull_position") {
        reply_position(m.req_id, net);
      }
    }
  }
};

/** @brief Make a non-blocking socketpair. */
void MakePair(int sp[2]) {
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
  for (int i = 0; i < 2; ++i) {
    const int fl = ::fcntl(sp[i], F_GETFL, 0);
    ::fcntl(sp[i], F_SETFL, fl | O_NONBLOCK);
  }
}

/** @brief A resting limit order. */
Order Limit(Side side, Ticks price, Quantity qty) {
  Order o;
  o.side = side;
  o.type = OrdType::Limit;
  o.tif = Tif::GTC;
  o.price = price;
  o.qty = qty;
  return o;
}

}  // namespace

TEST_CASE("Report-before-place keeps arrival order despite out-of-order acks",
          "[engine]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);
  StorageClient storage(client);
  MatchingEngine matching;
  TradingEngine engine(matching, storage);
  TestHelper helper{sp[1], codec, {}};

  std::string order;  // records the order in which placements complete
  const std::string a = engine.submit("BTC", "", Limit(Side::Buy, 100, 5),
                                      [&](const TradingEngine::Result& r) {
                                        if (r.storage_ok) order.push_back('A');
                                      });
  const std::string b = engine.submit("BTC", "", Limit(Side::Buy, 99, 5),
                                      [&](const TradingEngine::Result& r) {
                                        if (r.storage_ok) order.push_back('B');
                                      });
  REQUIRE(a != b);
  REQUIRE(engine.pending_placements() == 2);

  // The helper sees two report_order requests; acknowledge the SECOND first.
  std::vector<HelperMessage> reqs = helper.read_requests();
  REQUIRE(reqs.size() == 2);
  helper.ack(reqs[1].req_id, "report_order");
  for (int i = 0; i < 20 && engine.pending_placements() == 2; ++i) {
    loop.run_once(5);
  }
  // Order B's ack arrived but A is still first in line, so nothing placed yet.
  REQUIRE(order.empty());
  REQUIRE(engine.pending_placements() == 2);

  helper.ack(reqs[0].req_id, "report_order");
  for (int i = 0; i < 20 && engine.pending_placements() > 0; ++i) {
    loop.run_once(5);
  }
  REQUIRE(order == "AB");  // placed in arrival order, not ack order
  REQUIRE(matching.resting_count("BTC") == 2);

  ::close(sp[1]);
}

TEST_CASE("Trades and fills are reported to storage", "[engine]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);
  StorageClient storage(client);
  MatchingEngine matching;
  TradingEngine engine(matching, storage);
  TestHelper helper{sp[1], codec, {}};

  auto ack_pending_orders = [&]() {
    for (const HelperMessage& m : helper.read_requests()) {
      if (m.type == "report_order") {
        helper.ack(m.req_id, "report_order");
      }
    }
    for (int i = 0; i < 20; ++i) {
      loop.run_once(2);
    }
  };

  engine.submit("BTC", "", Limit(Side::Sell, 100, 5), nullptr);  // resting maker
  ack_pending_orders();
  engine.submit("BTC", "", Limit(Side::Buy, 100, 5), nullptr);   // crosses -> trade
  ack_pending_orders();

  // Collect everything the helper received and check the reporting.
  int trades = 0;
  int fills_filled = 0;
  for (const HelperMessage& m : helper.read_requests()) {
    if (m.type == "report_trade") {
      ++trades;
      REQUIRE(m.get("qty") != nullptr);
      REQUIRE(*m.get("qty") == "5");
      REQUIRE(m.get("taker") != nullptr);
      REQUIRE(m.get("maker") != nullptr);
    } else if (m.type == "report_fill") {
      if (m.get("status") != nullptr && *m.get("status") == "filled") {
        ++fills_filled;
      }
    }
  }
  REQUIRE(trades == 1);
  REQUIRE(fills_filled == 2);  // both taker and maker fully filled

  ::close(sp[1]);
}

TEST_CASE("A failed pre-report does not place the order", "[engine]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);
  StorageClient storage(client);
  MatchingEngine matching;
  TradingEngine engine(matching, storage);

  bool called = false;
  bool ok = true;
  engine.submit("BTC", "", Limit(Side::Buy, 100, 5),
                [&](const TradingEngine::Result& r) {
                  called = true;
                  ok = r.storage_ok;
                });

  ::close(sp[1]);  // helper dies before acknowledging
  for (int i = 0; i < 50 && !called; ++i) {
    loop.run_once(5);
  }
  REQUIRE(called);
  REQUIRE_FALSE(ok);
  REQUIRE(matching.resting_count("BTC") == 0);  // never placed
}

TEST_CASE("Trade and fill report failures are surfaced by the engine",
          "[engine]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec, /*timeout=*/30'000'000);
  StorageClient storage(client);
  MatchingEngine matching;
  TradingEngine engine(matching, storage);
  TestHelper helper{sp[1], codec, {}};

  // Ack only report_order messages so orders place, but never ack the
  // report_trade / report_fill that follow a match -- they must time out.
  auto ack_orders = [&]() {
    for (const HelperMessage& m : helper.read_requests()) {
      if (m.type == "report_order") {
        helper.ack(m.req_id, "report_order");
      }
    }
    for (int i = 0; i < 10; ++i) {
      loop.run_once(2);
    }
  };

  engine.submit("BTC", "", Limit(Side::Sell, 100, 5), nullptr);  // resting maker
  ack_orders();
  engine.submit("BTC", "", Limit(Side::Buy, 100, 5), nullptr);   // crosses -> 1 trade
  ack_orders();

  // The trade and the two fills (taker + maker) were reported but never
  // acknowledged; after the timeout the engine counts them as failures.
  for (int i = 0; i < 60 && engine.report_failures() < 3; ++i) {
    loop.run_once(20);
  }
  REQUIRE(engine.report_failures() == 3);

  ::close(sp[1]);
}

TEST_CASE("Owner may cancel by handle; a non-owner may not", "[engine][auth]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);
  StorageClient storage(client);
  MatchingEngine matching;
  TradingEngine engine(matching, storage);
  TestHelper helper{.fd = sp[1], .codec = codec, .in = {}};

  // Two distinct owners (valid v4 UUIDs).
  const std::string alice = "11111111-1111-4111-8111-111111111111";
  const std::string bob = "22222222-2222-4222-8222-222222222222";

  // Alice rests a limit order; the engine returns its opaque UUID handle.
  const std::string handle =
      engine.submit("BTC", alice, Limit(Side::Buy, 100, 5), nullptr);
  REQUIRE(IsValidUuidString(handle));
  // The engine pulls alice's position (first order) and reports the order;
  // answer both so it can place. Alice starts flat.
  helper.serve(/*net=*/0);
  for (int i = 0; i < 20 && engine.pending_placements() > 0; ++i) {
    loop.run_once(2);
    helper.serve(0);
  }
  REQUIRE(matching.resting_count("BTC") == 1);

  // An unknown handle is not found.
  REQUIRE(engine.cancel("33333333-3333-4333-8333-333333333333", alice) ==
          CancelResult::kNotFound);

  // Bob is not the owner: forbidden, and the order still rests.
  REQUIRE(engine.cancel(handle, bob) == CancelResult::kForbidden);
  REQUIRE(matching.resting_count("BTC") == 1);

  // Alice owns it: the cancel succeeds and reports the symbol back.
  Symbol symbol;
  REQUIRE(engine.cancel(handle, alice, &symbol) == CancelResult::kOk);
  REQUIRE(symbol == "BTC");
  REQUIRE(matching.resting_count("BTC") == 0);

  // The handle is gone after a successful cancel.
  REQUIRE(engine.cancel(handle, alice) == CancelResult::kNotFound);

  ::close(sp[1]);
}

TEST_CASE("A position pulled from storage caps a reduce-only order",
          "[engine][reduceonly]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);
  StorageClient storage(client);
  MatchingEngine matching;
  TradingEngine engine(matching, storage);
  TestHelper helper{.fd = sp[1], .codec = codec, .in = {}};

  const std::string carol = "33333333-3333-4333-8333-333333333333";

  // Carol has no book presence this session, but storage says she is long 10.
  // A reduce-only sell of 15 must be capped to that pulled position.
  Order ro = Limit(Side::Sell, 100, 15);
  SetFlag(&ro.flags, OrderFlag::kReduceOnly);
  TradingEngine::Result result;
  bool done = false;
  engine.submit("BTC", carol, ro, [&](const TradingEngine::Result& r) {
    result = r;
    done = true;
  });
  helper.serve(/*net=*/10);  // ack the report + answer the position pull (long 10)
  for (int i = 0; i < 40 && !done; ++i) {
    loop.run_once(2);
    helper.serve(10);
  }
  REQUIRE(done);
  REQUIRE(result.storage_ok);
  REQUIRE(result.outcome.accepted);
  REQUIRE(result.outcome.rested);

  const Order* placed = matching.find("BTC", result.order_id);
  REQUIRE(placed != nullptr);
  REQUIRE(placed->leaves == 10);  // capped to the pulled long, not the 15 asked

  ::close(sp[1]);
}
