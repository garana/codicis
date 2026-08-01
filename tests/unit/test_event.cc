/**
 * @file test_event.cc
 * @brief Unit tests for the codicis_event subsystem.
 *
 * Timer semantics are tested deterministically with FakeEventLoop + a
 * ManualClock (no sleeps). Real descriptor readiness is tested against the
 * platform backend via MakeEventLoop over pipes/socketpairs.
 */

#include "catch_amalgamated.hpp"

#include "codicis/event/event_loop.h"
#include "codicis/util/clock.h"
#include "fake_event_loop.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace codicis;

namespace {

/** @brief Records I/O callbacks and optionally stops a loop on first fire. */
struct RecordHandler : IoHandler {
  int count = 0;
  IoEvents last = IoEvents::kNone;
  EventLoop* stop_loop = nullptr;

  void on_io_ready(int /*fd*/, IoEvents events) override {
    ++count;
    last = events;
    if (stop_loop != nullptr) {
      stop_loop->stop();
    }
  }
};

/** @brief Records timer callbacks and optionally stops a loop on first fire. */
struct TimerRecord : TimerHandler {
  int count = 0;
  TimerId last = 0;
  EventLoop* stop_loop = nullptr;

  void on_timer(TimerId id) override {
    ++count;
    last = id;
    if (stop_loop != nullptr) {
      stop_loop->stop();
    }
  }
};

/** @brief Put a descriptor into non-blocking mode. */
void SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

TEST_CASE("One-shot timer fires once when the clock passes it",
          "[event][timer]") {
  ManualClock clock(0);
  FakeEventLoop loop(&clock);
  TimerRecord h;
  const TimerId id = loop.add_timer(100, /*repeat=*/false, &h);

  REQUIRE(loop.run_once(0).ok());
  REQUIRE(h.count == 0);  // clock not advanced yet

  clock.advance(100);
  REQUIRE(loop.run_once(0).ok());
  REQUIRE(h.count == 1);
  REQUIRE(h.last == id);

  clock.advance(1000);
  REQUIRE(loop.run_once(0).ok());
  REQUIRE(h.count == 1);  // one-shot does not re-fire
}

TEST_CASE("Repeating timer re-arms each interval", "[event][timer]") {
  ManualClock clock(0);
  FakeEventLoop loop(&clock);
  TimerRecord h;
  loop.add_timer(100, /*repeat=*/true, &h);

  clock.advance(100);
  REQUIRE(loop.run_once(0).ok());
  REQUIRE(h.count == 1);

  clock.advance(100);
  REQUIRE(loop.run_once(0).ok());
  REQUIRE(h.count == 2);
}

TEST_CASE("Cancelled timer never fires", "[event][timer]") {
  ManualClock clock(0);
  FakeEventLoop loop(&clock);
  TimerRecord h;
  const TimerId id = loop.add_timer(100, /*repeat=*/false, &h);
  loop.cancel_timer(id);

  clock.advance(1000);
  REQUIRE(loop.run_once(0).ok());
  REQUIRE(h.count == 0);
}

TEST_CASE("Earlier timer fires before a later one", "[event][timer]") {
  ManualClock clock(0);
  FakeEventLoop loop(&clock);
  TimerRecord early;
  TimerRecord late;
  loop.add_timer(100, false, &early);
  loop.add_timer(200, false, &late);

  clock.advance(100);
  REQUIRE(loop.run_once(0).ok());
  REQUIRE(early.count == 1);
  REQUIRE(late.count == 0);

  clock.advance(100);
  REQUIRE(loop.run_once(0).ok());
  REQUIRE(late.count == 1);
}

TEST_CASE("FakeEventLoop records interest and dispatches injected I/O",
          "[event][fake]") {
  ManualClock clock(0);
  FakeEventLoop loop(&clock);
  RecordHandler h;
  const int fd = 42;  // not a real descriptor; FakeEventLoop never touches it

  REQUIRE(loop.add(fd, IoInterest::kRead, &h).ok());
  REQUIRE(loop.interest_of(fd) == IoInterest::kRead);
  REQUIRE(loop.fd_count() == 1);

  SECTION("duplicate registration is a conflict") {
    Status s = loop.add(fd, IoInterest::kRead, &h);
    REQUIRE_FALSE(s.ok());
    REQUIRE(s.error().code == ErrorCode::kConflict);
  }

  SECTION("posted I/O is dispatched on poll") {
    loop.post_io(fd, IoEvents::kReadable);
    REQUIRE(loop.run_once(0).ok());
    REQUIRE(h.count == 1);
    REQUIRE(HasEvent(h.last, IoEvents::kReadable));
  }

  SECTION("modify updates interest") {
    REQUIRE(loop.modify(fd, IoInterest::kWrite).ok());
    REQUIRE(loop.interest_of(fd) == IoInterest::kWrite);
  }

  SECTION("removed fd no longer dispatches") {
    REQUIRE(loop.remove(fd).ok());
    REQUIRE(loop.interest_of(fd) == IoInterest::kNone);
    loop.post_io(fd, IoEvents::kReadable);
    REQUIRE(loop.run_once(0).ok());
    REQUIRE(h.count == 0);
  }
}

TEST_CASE("Real backend reports a readable pipe", "[event][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  SetNonBlocking(fds[0]);
  SetNonBlocking(fds[1]);

  RecordHandler h;
  REQUIRE(loop.add(fds[0], IoInterest::kRead, &h).ok());

  const char byte = 'x';
  REQUIRE(::write(fds[1], &byte, 1) == 1);

  REQUIRE(loop.run_once(500).ok());
  REQUIRE(h.count >= 1);
  REQUIRE(HasEvent(h.last, IoEvents::kReadable));

  ::close(fds[0]);
  ::close(fds[1]);
}

TEST_CASE("Real backend enables writability only after modify",
          "[event][backend]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  int sp[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
  SetNonBlocking(sp[0]);
  SetNonBlocking(sp[1]);

  RecordHandler h;
  // Watch reads only: nothing to read, so a short poll must not dispatch.
  REQUIRE(loop.add(sp[0], IoInterest::kRead, &h).ok());
  REQUIRE(loop.run_once(50).ok());
  REQUIRE(h.count == 0);

  // Now ask for writability: the socket has send buffer space immediately.
  REQUIRE(loop.modify(sp[0], IoInterest::kWrite).ok());
  REQUIRE(loop.run_once(500).ok());
  REQUIRE(h.count >= 1);
  REQUIRE(HasEvent(h.last, IoEvents::kWritable));

  ::close(sp[0]);
  ::close(sp[1]);
}

TEST_CASE("Real backend run() returns after a timer stops it",
          "[event][backend][timer]") {
  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> lr = MakeEventLoop(&clock);
  REQUIRE(lr.ok());
  EventLoop& loop = *lr.value();

  TimerRecord t;
  t.stop_loop = &loop;
  loop.add_timer(5'000'000 /*5ms*/, /*repeat=*/false, &t);

  REQUIRE(loop.run().ok());
  REQUIRE(t.count == 1);
}
