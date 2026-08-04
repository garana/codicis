/**
 * @file test_ipc.cc
 * @brief Tests for helper codecs, the pipelined client, and StorageClient.
 */

#include "catch_amalgamated.hpp"

#include "codicis/event/event_loop.h"
#include "codicis/ipc/helper_client.h"
#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/helper_message.h"
#include "codicis/ipc/storage_client.h"
#include "codicis/util/buffer.h"
#include "codicis/util/clock.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace codicis;

namespace {

/** @brief Emulates the helper end of a socketpair for tests. */
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
      const HelperDecode d = codec.decode(in, &m, &err);
      if (d != HelperDecode::kComplete) {
        break;
      }
      out.push_back(std::move(m));
    }
    return out;
  }

  /** @brief Encode and write a response. */
  void send_response(const HelperMessage& msg) {
    std::string bytes;
    codec.encode(msg, &bytes);
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

}  // namespace

TEST_CASE("Codecs round-trip a message", "[ipc][codec]") {
  HelperMessage msg;
  msg.req_id = 42;
  msg.type = "report_order";
  msg.set("id", "1001");
  msg.set("px", "-5");  // value with characters that must survive

  auto check = [&](const HelperCodec& codec) {
    std::string bytes;
    codec.encode(msg, &bytes);
    Buffer b;
    b.append(bytes);
    HelperMessage out;
    std::string err;
    REQUIRE(codec.decode(b, &out, &err) == HelperDecode::kComplete);
    REQUIRE(out.req_id == 42);
    REQUIRE(out.type == "report_order");
    REQUIRE(out.get("id") != nullptr);
    REQUIRE(*out.get("id") == "1001");
    REQUIRE(*out.get("px") == "-5");
    REQUIRE(b.empty());
  };
  check(TextHelperCodec{});
  check(BinaryHelperCodec{});
}

TEST_CASE("Codecs decode several pipelined messages", "[ipc][codec]") {
  auto check = [&](const HelperCodec& codec) {
    HelperMessage a;
    a.req_id = 1;
    a.type = "ping";
    HelperMessage b;
    b.req_id = 2;
    b.type = "commit";
    std::string bytes;
    codec.encode(a, &bytes);
    codec.encode(b, &bytes);

    Buffer buf;
    buf.append(bytes);
    HelperMessage out;
    std::string err;
    REQUIRE(codec.decode(buf, &out, &err) == HelperDecode::kComplete);
    REQUIRE(out.req_id == 1);
    REQUIRE(codec.decode(buf, &out, &err) == HelperDecode::kComplete);
    REQUIRE(out.req_id == 2);
    REQUIRE(codec.decode(buf, &out, &err) == HelperDecode::kIncomplete);
  };
  check(TextHelperCodec{});
  check(BinaryHelperCodec{});
}

TEST_CASE("Binary codec rejects a bad magic", "[ipc][codec]") {
  BinaryHelperCodec codec;
  Buffer b;
  b.append(std::string("XXXXYYYY"));  // 8 bytes, wrong 4-byte magic
  HelperMessage out;
  std::string err;
  REQUIRE(codec.decode(b, &out, &err) == HelperDecode::kError);
}

TEST_CASE("Codecs reject adversarial input", "[ipc][codec][hardening]") {
  HelperMessage out;
  std::string err;

  SECTION("binary codec rejects an oversize declared payload") {
    // Valid magic, then a payload length far beyond the 64 MiB cap.
    std::string wire;
    wire.push_back('C');
    wire.push_back('O');
    wire.push_back('D');
    wire.push_back('C');
    for (int i = 0; i < 4; ++i) {
      wire.push_back(static_cast<char>(0xFF));  // len = 0xFFFFFFFF
    }
    Buffer b;
    b.append(wire);
    BinaryHelperCodec codec;
    REQUIRE(codec.decode(b, &out, &err) == HelperDecode::kError);
  }

  SECTION("binary codec rejects a field length that overruns the payload") {
    // magic + payload_len covering: req_id(8) type_len(2)=0 count(4)=1
    // key_len(4) = huge, so the field key overruns the declared payload.
    std::string payload;
    for (int i = 0; i < 8; ++i) payload.push_back(0);  // req_id
    payload.push_back(0);
    payload.push_back(0);  // type_len = 0
    payload.push_back(1);
    payload.push_back(0);
    payload.push_back(0);
    payload.push_back(0);  // count = 1
    payload.push_back(static_cast<char>(0xFF));
    payload.push_back(static_cast<char>(0xFF));
    payload.push_back(static_cast<char>(0xFF));
    payload.push_back(0x7F);  // key_len ~ 2 GiB
    std::string wire;
    wire.push_back('C');
    wire.push_back('O');
    wire.push_back('D');
    wire.push_back('C');
    const std::uint32_t plen = static_cast<std::uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) {
      wire.push_back(static_cast<char>((plen >> (8 * i)) & 0xFF));
    }
    wire += payload;
    Buffer b;
    b.append(wire);
    BinaryHelperCodec codec;
    REQUIRE(codec.decode(b, &out, &err) == HelperDecode::kError);
  }

  SECTION("text codec rejects a non-numeric req_id") {
    Buffer b;
    b.append(std::string("req_id=notanumber\ntype=x\n\n"));
    TextHelperCodec codec;
    REQUIRE(codec.decode(b, &out, &err) == HelperDecode::kError);
  }

  SECTION("text codec rejects an overflowing req_id") {
    Buffer b;
    b.append(std::string("req_id=99999999999999999999\ntype=x\n\n"));
    TextHelperCodec codec;
    REQUIRE(codec.decode(b, &out, &err) == HelperDecode::kError);
  }

  SECTION("text codec rejects a line without '='") {
    Buffer b;
    b.append(std::string("req_id=1\ngarbage\n\n"));
    TextHelperCodec codec;
    REQUIRE(codec.decode(b, &out, &err) == HelperDecode::kError);
  }
}

TEST_CASE("HelperClient correlates pipelined out-of-order responses",
          "[ipc][client]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);
  TestHelper helper{sp[1], codec, {}};

  std::string got_a;
  std::string got_b;
  client.send("q", {{"which", "a"}},
              [&](bool ok, const HelperMessage& reply) {
                if (ok) got_a = *reply.get("which");
              });
  client.send("q", {{"which", "b"}},
              [&](bool ok, const HelperMessage& reply) {
                if (ok) got_b = *reply.get("which");
              });

  // Helper sees both requests; reply in reverse order.
  std::vector<HelperMessage> reqs = helper.read_requests();
  REQUIRE(reqs.size() == 2);
  for (auto it = reqs.rbegin(); it != reqs.rend(); ++it) {
    HelperMessage resp;
    resp.req_id = it->req_id;
    resp.type = "r";
    resp.set("which", *it->get("which"));
    helper.send_response(resp);
  }

  for (int i = 0; i < 20 && (got_a.empty() || got_b.empty()); ++i) {
    loop.run_once(5);
  }
  REQUIRE(got_a == "a");
  REQUIRE(got_b == "b");
  REQUIRE(client.pending_count() == 0);

  ::close(sp[1]);
}

TEST_CASE("StorageClient tracks outbox and commit watermark",
          "[ipc][storage]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);
  StorageClient storage(client);
  TestHelper helper{sp[1], codec, {}};

  std::uint64_t highest = 0;

  bool order_ok = false;
  storage.report_order({{"id", "1"}}, [&](bool ok) { order_ok = ok; });
  REQUIRE(storage.processed_pending() == 1);

  // Service the report.
  for (const HelperMessage& req : helper.read_requests()) {
    highest = std::max(highest, req.req_id);
    HelperMessage resp;
    resp.req_id = req.req_id;
    resp.type = req.type;
    resp.set("status", "ok");
    helper.send_response(resp);
  }
  for (int i = 0; i < 20 && !order_ok; ++i) {
    loop.run_once(5);
  }
  REQUIRE(order_ok);
  REQUIRE(storage.processed_pending() == 1);  // acked, not yet committed

  // Commit clears the outbox up to the watermark.
  bool committed = false;
  std::uint64_t watermark = 0;
  storage.commit([&](bool ok, std::uint64_t w) {
    committed = ok;
    watermark = w;
  });
  for (const HelperMessage& req : helper.read_requests()) {
    HelperMessage resp;
    resp.req_id = req.req_id;
    resp.type = "commit";
    resp.set("committed", std::to_string(highest));
    helper.send_response(resp);
  }
  for (int i = 0; i < 20 && !committed; ++i) {
    loop.run_once(5);
  }
  REQUIRE(committed);
  REQUIRE(watermark == 1);
  REQUIRE(storage.processed_pending() == 0);

  ::close(sp[1]);
}

#if defined(CODICIS_STORAGE_HELPER_PATH)
TEST_CASE("StorageClient works against the spawned reference helper",
          "[ipc][spawn]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  Result<std::unique_ptr<HelperClient>> cr =
      SpawnHelper(loop, {CODICIS_STORAGE_HELPER_PATH}, codec);
  REQUIRE(cr.ok());
  StorageClient storage(*cr.value());

  bool order_ok = false;
  storage.report_order({{"id", "7"}, {"px", "100"}},
                       [&](bool ok) { order_ok = ok; });
  for (int i = 0; i < 200 && !order_ok; ++i) {
    loop.run_once(5);
  }
  REQUIRE(order_ok);

  bool committed = false;
  storage.commit([&](bool ok, std::uint64_t) { committed = ok; });
  for (int i = 0; i < 200 && !committed; ++i) {
    loop.run_once(5);
  }
  REQUIRE(committed);
  REQUIRE(storage.processed_pending() == 0);
}

TEST_CASE("Storage helper accumulates positions and answers pull_position",
          "[ipc][spawn][position]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  Result<std::unique_ptr<HelperClient>> cr =
      SpawnHelper(loop, {CODICIS_STORAGE_HELPER_PATH}, codec);
  REQUIRE(cr.ok());
  StorageClient storage(*cr.value());

  const std::string alice = "aaaa1111-1111-4111-8111-111111111111";
  const std::string bob = "bbbb2222-2222-4222-8222-222222222222";

  auto pump = [&](const bool& done) {
    for (int i = 0; i < 200 && !done; ++i) {
      loop.run_once(5);
    }
  };

  // Bob (seller, id 1) trades 10 against Alice (buyer, id 2): the helper learns
  // each order's owner+side from report_order, then attributes the fills.
  bool o1 = false, o2 = false;
  storage.report_order(
      {{"owner", bob}, {"id", "1"}, {"side", "sell"}, {"symbol", "BTC"}},
      [&](bool ok) { o1 = ok; });
  storage.report_order(
      {{"owner", alice}, {"id", "2"}, {"side", "buy"}, {"symbol", "BTC"}},
      [&](bool ok) { o2 = ok; });
  pump(o2);
  REQUIRE(o1);
  REQUIRE(o2);

  bool f1 = false, f2 = false;
  storage.report_fill({{"id", "2"}, {"qty", "10"}, {"symbol", "BTC"}},
                      [&](bool ok) { f1 = ok; });
  storage.report_fill({{"id", "1"}, {"qty", "10"}, {"symbol", "BTC"}},
                      [&](bool ok) { f2 = ok; });
  pump(f2);
  REQUIRE(f1);
  REQUIRE(f2);

  // Alice is now long 10, Bob short 10, and an untouched account is flat.
  std::int64_t a_net = 0, b_net = 0, c_net = 999;
  bool a_done = false, b_done = false, c_done = false;
  storage.pull_position(alice, "BTC", [&](bool ok, std::int64_t net) {
    a_net = net;
    a_done = ok;
  });
  storage.pull_position(bob, "BTC", [&](bool ok, std::int64_t net) {
    b_net = net;
    b_done = ok;
  });
  storage.pull_position("cccc3333-3333-4333-8333-333333333333", "BTC",
                        [&](bool ok, std::int64_t net) {
                          c_net = net;
                          c_done = ok;
                        });
  pump(c_done);
  REQUIRE(a_done);
  REQUIRE(b_done);
  REQUIRE(c_done);
  REQUIRE(a_net == 10);   // long
  REQUIRE(b_net == -10);  // short
  REQUIRE(c_net == 0);    // flat
}

TEST_CASE("Storage helper serves the resting book via pull_levels",
          "[ipc][spawn][deep]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  Result<std::unique_ptr<HelperClient>> cr =
      SpawnHelper(loop, {CODICIS_STORAGE_HELPER_PATH}, codec);
  REQUIRE(cr.ok());
  StorageClient storage(*cr.value());

  auto pump = [&](const bool& done) {
    for (int i = 0; i < 200 && !done; ++i) {
      loop.run_once(5);
    }
  };

  // Two resting bids at 100 (arrival order 1 then 2) and a deeper one at 99.
  bool d = false;
  storage.report_rest("BTC", "buy", 1, 100, 5, 1, nullptr);
  storage.report_rest("BTC", "buy", 2, 100, 3, 2, nullptr);
  storage.report_rest("BTC", "buy", 3, 99, 4, 3, [&](bool) { d = true; });
  pump(d);

  // Pull the best 2 deep bid levels below 101: level 100 (ids 1,2 in arrival
  // order) then level 99 (id 3).
  std::vector<PulledOrder> got;
  bool p = false;
  storage.pull_levels("BTC", "buy", /*from_price=*/101, /*count=*/2,
                      [&](bool ok, std::vector<PulledOrder> o) {
                        got = std::move(o);
                        p = ok;
                      });
  pump(p);
  REQUIRE(p);
  REQUIRE(got.size() == 3);
  REQUIRE(got[0].id == 1);   // best price first...
  REQUIRE(got[0].price == 100);
  REQUIRE(got[1].id == 2);   // ...then arrival order within the level
  REQUIRE(got[1].seq == 2);
  REQUIRE(got[2].id == 3);   // deeper level last
  REQUIRE(got[2].price == 99);
  REQUIRE(got[2].leaves == 4);

  // A count of 1 returns only the best level.
  std::vector<PulledOrder> one;
  bool p2 = false;
  storage.pull_levels("BTC", "buy", 101, 1,
                      [&](bool ok, std::vector<PulledOrder> o) {
                        one = std::move(o);
                        p2 = ok;
                      });
  pump(p2);
  REQUIRE(one.size() == 2);  // both orders at price 100
  REQUIRE(one[0].price == 100);

  // Cancelling id 2 drops it from the level.
  bool r = false;
  storage.report_cancel("BTC", 2, [&](bool) { r = true; });
  pump(r);
  std::vector<PulledOrder> after;
  bool p3 = false;
  storage.pull_levels("BTC", "buy", 101, 2,
                      [&](bool ok, std::vector<PulledOrder> o) {
                        after = std::move(o);
                        p3 = ok;
                      });
  pump(p3);
  REQUIRE(after.size() == 2);  // id 1 at 100, id 3 at 99
  REQUIRE(after[0].id == 1);
  REQUIRE(after[1].id == 3);

  // A fill decrements a resting order's leaves, and a full fill removes it.
  // First report id 1's owner/side so report_fill is attributed, then fill 2.
  bool f = false;
  storage.report_order(
      {{"owner", "u"}, {"id", "1"}, {"side", "buy"}, {"symbol", "BTC"}},
      nullptr);
  storage.report_fill({{"symbol", "BTC"}, {"id", "1"}, {"qty", "2"}},
                      [&](bool) { f = true; });
  pump(f);
  std::vector<PulledOrder> filled;
  bool p4 = false;
  storage.pull_levels("BTC", "buy", 101, 2,
                      [&](bool ok, std::vector<PulledOrder> o) {
                        filled = std::move(o);
                        p4 = ok;
                      });
  pump(p4);
  REQUIRE(filled.size() == 2);
  REQUIRE(filled[0].id == 1);
  REQUIRE(filled[0].leaves == 3);  // 5 - 2 filled

  bool f2 = false;
  storage.report_fill({{"symbol", "BTC"}, {"id", "1"}, {"qty", "3"}},
                      [&](bool) { f2 = true; });
  pump(f2);
  std::vector<PulledOrder> gone;
  bool p5 = false;
  storage.pull_levels("BTC", "buy", 101, 2,
                      [&](bool ok, std::vector<PulledOrder> o) {
                        gone = std::move(o);
                        p5 = ok;
                      });
  pump(p5);
  REQUIRE(gone.size() == 1);   // id 1 fully filled and removed; only id 3 left
  REQUIRE(gone[0].id == 3);
}
#endif

TEST_CASE("HelperClient times out a request with no response", "[ipc][timeout]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec, /*timeout=*/30'000'000);  // 30ms

  bool called = false;
  bool ok = true;
  client.send("ping", {}, [&](bool o, const HelperMessage&) {
    called = true;
    ok = o;
  });
  REQUIRE(client.pending_count() == 1);

  // The helper end never responds; the request must time out and fail.
  for (int i = 0; i < 50 && !called; ++i) {
    loop.run_once(20);
  }
  REQUIRE(called);
  REQUIRE_FALSE(ok);
  REQUIRE(client.pending_count() == 0);

  ::close(sp[1]);
}

TEST_CASE("HelperClient fails an in-flight request when the helper dies",
          "[ipc][close]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);

  bool called = false;
  bool ok = true;
  client.send("ping", {}, [&](bool o, const HelperMessage&) {
    called = true;
    ok = o;
  });
  REQUIRE(client.pending_count() == 1);

  ::close(sp[1]);  // helper dies before responding
  for (int i = 0; i < 20 && !called; ++i) {
    loop.run_once(10);
  }
  REQUIRE(called);
  REQUIRE_FALSE(ok);
  REQUIRE(client.closed());
}

TEST_CASE("HelperClient fails, not drops, a request sent after close",
          "[ipc][close]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);

  ::close(sp[1]);
  for (int i = 0; i < 20 && !client.closed(); ++i) {
    loop.run_once(10);
  }
  REQUIRE(client.closed());

  bool called = false;
  bool ok = true;
  client.send("ping", {}, [&](bool o, const HelperMessage&) {
    called = true;
    ok = o;
  });
  REQUIRE(called);        // invoked immediately
  REQUIRE_FALSE(ok);
}

TEST_CASE("StorageClient commit fails and keeps the outbox when helper dies",
          "[ipc][storage]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TextHelperCodec codec;
  int sp[2];
  MakePair(sp);
  HelperClient client(loop, sp[0], sp[0], codec);
  StorageClient storage(client);
  TestHelper helper{sp[1], codec, {}};

  bool acked = false;
  storage.report_order({{"id", "1"}}, [&](bool ok) { acked = ok; });
  for (const HelperMessage& m : helper.read_requests()) {
    helper.send_response([&] {
      HelperMessage r;
      r.req_id = m.req_id;
      r.type = m.type;
      r.set("status", "ok");
      return r;
    }());
  }
  for (int i = 0; i < 20 && !acked; ++i) {
    loop.run_once(5);
  }
  REQUIRE(acked);
  REQUIRE(storage.processed_pending() == 1);

  ::close(sp[1]);  // helper dies
  for (int i = 0; i < 20 && !client.closed(); ++i) {
    loop.run_once(5);
  }

  bool committed = false;
  bool cok = true;
  storage.commit([&](bool ok, std::uint64_t) {
    committed = true;
    cok = ok;
  });
  for (int i = 0; i < 20 && !committed; ++i) {
    loop.run_once(5);
  }
  REQUIRE(committed);
  REQUIRE_FALSE(cok);
  REQUIRE(storage.processed_pending() == 1);  // not dropped: commit unconfirmed
}
