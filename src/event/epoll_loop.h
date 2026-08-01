#ifndef CODICIS_EVENT_EPOLL_LOOP_H
#define CODICIS_EVENT_EPOLL_LOOP_H

/**
 * @file epoll_loop.h
 * @brief epoll-based EventLoop backend (Linux).
 *
 * Internal to the event subsystem. Carries no platform includes; the epoll
 * descriptor is held as a plain int.
 */

#include <memory>

#include "codicis/event/event_loop.h"
#include "codicis/util/clock.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief EventLoop backend using epoll(7), level-triggered.
 */
class EpollLoop final : public EventLoop {
 public:
  /**
   * @brief Create an epoll loop.
   * @param clock Time source for timers (must outlive the loop).
   * @return The loop, or an Error if epoll_create1() fails.
   */
  static Result<std::unique_ptr<EpollLoop>> Create(Clock* clock);

  ~EpollLoop() override;

 protected:
  Status backend_add(int fd, IoInterest interest) override;
  Status backend_modify(int fd, IoInterest interest) override;
  Status backend_remove(int fd) override;
  Status backend_poll(int timeout_ms) override;

 private:
  /**
   * @brief Construct with an owned epoll descriptor.
   * @param clock Time source.
   * @param epfd  An open epoll descriptor.
   */
  EpollLoop(Clock* clock, int epfd) : EventLoop(clock), epfd_(epfd) {}

  int epfd_;
};

}  // namespace codicis

#endif  // CODICIS_EVENT_EPOLL_LOOP_H
