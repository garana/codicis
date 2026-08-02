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

  /** @brief Acknowledge a request by echoing its id with status ok. */
  void ack(std::uint64_t req_id, const std::string& type) {
    HelperMessage resp;
    resp.req_id = req_id;
    resp.type = type;
    resp.set("status", "ok");
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
  const OrderId a = engine.submit("BTC", Limit(Side::Buy, 100, 5),
                                  [&](const TradingEngine::Result& r) {
                                    if (r.storage_ok) order.push_back('A');
                                  });
  const OrderId b = engine.submit("BTC", Limit(Side::Buy, 99, 5),
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

  engine.submit("BTC", Limit(Side::Sell, 100, 5), nullptr);  // resting maker
  ack_pending_orders();
  engine.submit("BTC", Limit(Side::Buy, 100, 5), nullptr);   // crosses -> trade
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
  engine.submit("BTC", Limit(Side::Buy, 100, 5),
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

  engine.submit("BTC", Limit(Side::Sell, 100, 5), nullptr);  // resting maker
  ack_orders();
  engine.submit("BTC", Limit(Side::Buy, 100, 5), nullptr);   // crosses -> 1 trade
  ack_orders();

  // The trade and the two fills (taker + maker) were reported but never
  // acknowledged; after the timeout the engine counts them as failures.
  for (int i = 0; i < 60 && engine.report_failures() < 3; ++i) {
    loop.run_once(20);
  }
  REQUIRE(engine.report_failures() == 3);

  ::close(sp[1]);
}
