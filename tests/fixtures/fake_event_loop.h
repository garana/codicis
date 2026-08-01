#ifndef CODICIS_TESTS_FIXTURES_FAKE_EVENT_LOOP_H
#define CODICIS_TESTS_FIXTURES_FAKE_EVENT_LOOP_H

/**
 * @file fake_event_loop.h
 * @brief A deterministic EventLoop for unit-testing higher layers.
 *
 * FakeEventLoop performs no real I/O. Descriptor registrations are recorded so
 * tests can assert on interest, and I/O readiness is injected explicitly via
 * @ref post_io / @ref fire_io. Combined with a @ref ManualClock, timer
 * behavior is fully deterministic (no sleeps): advance the clock, then call
 * @ref EventLoop::run_once.
 */

#include <unordered_map>
#include <vector>

#include "codicis/event/event_loop.h"

namespace codicis {

/**
 * @brief A no-real-I/O EventLoop backend for tests.
 */
class FakeEventLoop final : public EventLoop {
 public:
  /**
   * @brief Construct with a (typically Manual) clock.
   * @param clock Time source (must outlive the loop).
   */
  explicit FakeEventLoop(Clock* clock) : EventLoop(clock) {}

  /**
   * @brief Queue an I/O event to be dispatched on the next poll.
   * @param fd     The descriptor.
   * @param events The events to deliver.
   */
  void post_io(int fd, IoEvents events) {
    pending_.push_back({fd, events});
  }

  /**
   * @brief Dispatch an I/O event immediately (outside a poll).
   * @param fd     The descriptor.
   * @param events The events to deliver.
   */
  void fire_io(int fd, IoEvents events) { dispatch_io(fd, events); }

  /**
   * @brief The interest last registered for @p fd, or kNone if unregistered.
   * @param fd The descriptor.
   * @return The recorded interest mask.
   */
  IoInterest interest_of(int fd) const {
    const auto it = interests_.find(fd);
    return it == interests_.end() ? IoInterest::kNone : it->second;
  }

 protected:
  Status backend_add(int fd, IoInterest interest) override {
    interests_[fd] = interest;
    return Status::Ok();
  }

  Status backend_modify(int fd, IoInterest interest) override {
    interests_[fd] = interest;
    return Status::Ok();
  }

  Status backend_remove(int fd) override {
    interests_.erase(fd);
    return Status::Ok();
  }

  Status backend_poll(int /*timeout_ms*/) override {
    std::vector<PendingIo> batch;
    batch.swap(pending_);
    for (const PendingIo& io : batch) {
      dispatch_io(io.fd, io.events);
    }
    return Status::Ok();
  }

 private:
  /** @brief A queued I/O readiness injection. */
  struct PendingIo {
    int fd;
    IoEvents events;
  };

  std::unordered_map<int, IoInterest> interests_;
  std::vector<PendingIo> pending_;
};

}  // namespace codicis

#endif  // CODICIS_TESTS_FIXTURES_FAKE_EVENT_LOOP_H
