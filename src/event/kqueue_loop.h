#ifndef CODICIS_EVENT_KQUEUE_LOOP_H
#define CODICIS_EVENT_KQUEUE_LOOP_H

/**
 * @file kqueue_loop.h
 * @brief kqueue/kevent-based EventLoop backend (BSD/macOS).
 *
 * Internal to the event subsystem. This header carries no platform includes;
 * the kqueue descriptor is held as a plain int so callers need not pull in
 * <sys/event.h>.
 */

#include <memory>

#include "codicis/event/event_loop.h"
#include "codicis/util/clock.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief EventLoop backend using kqueue(2)/kevent(2).
 *
 * Read and write interest are modeled as two per-fd filters (EVFILT_READ,
 * EVFILT_WRITE) that are enabled/disabled to match the requested interest;
 * the loop is level-triggered (no EV_CLEAR).
 */
class KqueueLoop final : public EventLoop {
 public:
  /**
   * @brief Create a kqueue loop.
   * @param clock Time source for timers (must outlive the loop).
   * @return The loop, or an Error if kqueue() fails.
   */
  static Result<std::unique_ptr<KqueueLoop>> Create(Clock* clock);

  ~KqueueLoop() override;

 protected:
  Status backend_add(int fd, IoInterest interest) override;
  Status backend_modify(int fd, IoInterest interest) override;
  Status backend_remove(int fd) override;
  Status backend_poll(int timeout_ms) override;

 private:
  /**
   * @brief Construct with an owned kqueue descriptor.
   * @param clock Time source.
   * @param kq    An open kqueue descriptor.
   */
  KqueueLoop(Clock* clock, int kq) : EventLoop(clock), kq_(kq) {}

  /**
   * @brief (Re)register the two filters for @p fd, matching @p interest.
   * @param fd       The descriptor.
   * @param interest The desired readiness.
   * @return Ok, or an Error on kevent() failure.
   */
  Status apply_interest(int fd, IoInterest interest);

  int kq_;
};

}  // namespace codicis

#endif  // CODICIS_EVENT_KQUEUE_LOOP_H
