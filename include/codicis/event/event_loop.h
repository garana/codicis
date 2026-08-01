#ifndef CODICIS_EVENT_EVENT_LOOP_H
#define CODICIS_EVENT_EVENT_LOOP_H

/**
 * @file event_loop.h
 * @brief Single-threaded I/O event loop abstraction.
 *
 * One @ref codicis::EventLoop multiplexes all file descriptors in the process
 * (listening/connected sockets and helper pipes) plus timers. Concrete
 * backends implement the platform primitive: @c KqueueLoop (BSD/macOS) and
 * @c EpollLoop (Linux). The loop presents a **level-triggered** contract on
 * both backends, which keeps hand-rolled partial-read/write logic simple.
 *
 * Callbacks are delivered through the lightweight @ref codicis::IoHandler and
 * @ref codicis::TimerHandler interfaces (no std::function on the hot path);
 * subsystems such as connections and helper clients implement them directly.
 */

#include <cstdint>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codicis/util/clock.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief Bitmask of the I/O readiness a caller wants to watch for.
 */
enum class IoInterest : unsigned {
  kNone = 0,
  kRead = 1u << 0,
  kWrite = 1u << 1,
};

/** @brief Bitwise OR of two interest masks. */
inline IoInterest operator|(IoInterest a, IoInterest b) {
  return static_cast<IoInterest>(static_cast<unsigned>(a) |
                                 static_cast<unsigned>(b));
}

/** @brief Bitwise AND of two interest masks. */
inline IoInterest operator&(IoInterest a, IoInterest b) {
  return static_cast<IoInterest>(static_cast<unsigned>(a) &
                                 static_cast<unsigned>(b));
}

/** @return True if @p mask has any bit of @p bit set. */
inline bool HasInterest(IoInterest mask, IoInterest bit) {
  return (static_cast<unsigned>(mask) & static_cast<unsigned>(bit)) != 0;
}

/**
 * @brief Bitmask of I/O conditions reported for a ready descriptor.
 */
enum class IoEvents : unsigned {
  kNone = 0,
  kReadable = 1u << 0,  /**< Data available or EOF pending. */
  kWritable = 1u << 1,  /**< Space available to write. */
  kError = 1u << 2,     /**< An error condition on the descriptor. */
  kHangup = 1u << 3,    /**< The peer closed / hung up. */
};

/** @brief Bitwise OR of two event masks. */
inline IoEvents operator|(IoEvents a, IoEvents b) {
  return static_cast<IoEvents>(static_cast<unsigned>(a) |
                               static_cast<unsigned>(b));
}

/** @brief In-place bitwise OR. */
inline IoEvents& operator|=(IoEvents& a, IoEvents b) {
  a = a | b;
  return a;
}

/** @return True if @p mask has any bit of @p bit set. */
inline bool HasEvent(IoEvents mask, IoEvents bit) {
  return (static_cast<unsigned>(mask) & static_cast<unsigned>(bit)) != 0;
}

/** @brief Opaque timer identifier (0 is never a valid timer). */
using TimerId = std::uint64_t;

/**
 * @brief Receives I/O readiness notifications for a registered descriptor.
 */
class IoHandler {
 public:
  virtual ~IoHandler() = default;

  /**
   * @brief Called when @p fd is ready with the given @p events.
   * @param fd     The descriptor that is ready.
   * @param events The set of conditions that became ready.
   */
  virtual void on_io_ready(int fd, IoEvents events) = 0;
};

/**
 * @brief Receives timer expiry notifications.
 */
class TimerHandler {
 public:
  virtual ~TimerHandler() = default;

  /**
   * @brief Called when a timer fires.
   * @param id The identifier returned by @ref EventLoop::add_timer.
   */
  virtual void on_timer(TimerId id) = 0;
};

/**
 * @brief Abstract, single-threaded I/O + timer multiplexer.
 *
 * The base class owns descriptor bookkeeping and the timer min-heap; concrete
 * subclasses implement only the platform poll/registration primitives. Not
 * thread-safe: all methods must be called from the loop's own thread.
 */
class EventLoop {
 public:
  /**
   * @brief Construct with a clock used for timer scheduling.
   * @param clock Time source (must outlive the loop).
   */
  explicit EventLoop(Clock* clock) : clock_(clock) {}

  virtual ~EventLoop() = default;

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  /**
   * @brief Register @p fd for @p interest, dispatching to @p handler.
   * @param fd       A non-blocking file descriptor.
   * @param interest The readiness to watch for.
   * @param handler  Callback (must outlive the registration).
   * @return Ok, or an Error (e.g. already registered, syscall failure).
   */
  Status add(int fd, IoInterest interest, IoHandler* handler);

  /**
   * @brief Change the interest mask for an already-registered @p fd.
   * @param fd       The descriptor.
   * @param interest The new readiness to watch for.
   * @return Ok, or an Error if @p fd is not registered.
   */
  Status modify(int fd, IoInterest interest);

  /**
   * @brief Unregister @p fd from the loop.
   * @param fd The descriptor.
   * @return Ok, or an Error if @p fd is not registered.
   */
  Status remove(int fd);

  /**
   * @brief Schedule a timer.
   * @param delay_ns Nanoseconds from now until the first expiry.
   * @param repeat   If true, reschedule every @p delay_ns after firing.
   * @param handler  Callback (must outlive the timer).
   * @return The new timer's id (never 0).
   */
  TimerId add_timer(Nanos delay_ns, bool repeat, TimerHandler* handler);

  /**
   * @brief Cancel a previously scheduled timer.
   * @param id The timer id; unknown/expired ids are ignored.
   */
  void cancel_timer(TimerId id);

  /**
   * @brief Run one iteration: wait for I/O or the next timer, then dispatch.
   * @param max_wait_ms Upper bound on the blocking wait, or -1 for "until the
   *                    next timer / an event" (no artificial cap).
   * @return Ok, or an Error from the underlying poll.
   */
  Status run_once(int max_wait_ms);

  /**
   * @brief Run iterations until @ref stop is called.
   * @return Ok on a clean stop, or the first Error encountered.
   */
  Status run();

  /** @brief Request that @ref run return after the current iteration. */
  void stop() { running_ = false; }

  /** @return The number of descriptors currently registered. */
  std::size_t fd_count() const { return registrations_.size(); }

  /** @return The clock used for timer scheduling. */
  Clock* clock() const { return clock_; }

 protected:
  /**
   * @brief Look up the handler for @p fd and deliver @p events to it.
   *
   * Called by backends from within @ref backend_poll. Safe if @p fd was
   * removed concurrently during dispatch (the event is dropped).
   * @param fd     The ready descriptor.
   * @param events The conditions that became ready.
   */
  void dispatch_io(int fd, IoEvents events);

  // ---- Platform primitives implemented by each backend -------------------

  /**
   * @brief Register @p fd with the platform poller for @p interest.
   * @param fd       The descriptor.
   * @param interest The readiness to watch.
   * @return Ok, or an Error on syscall failure.
   */
  virtual Status backend_add(int fd, IoInterest interest) = 0;

  /**
   * @brief Update the platform poller's interest for @p fd.
   * @param fd       The descriptor.
   * @param interest The new readiness to watch.
   * @return Ok, or an Error on syscall failure.
   */
  virtual Status backend_modify(int fd, IoInterest interest) = 0;

  /**
   * @brief Remove @p fd from the platform poller.
   * @param fd The descriptor.
   * @return Ok, or an Error on syscall failure.
   */
  virtual Status backend_remove(int fd) = 0;

  /**
   * @brief Wait up to @p timeout_ms and dispatch ready descriptors.
   *
   * Implementations call @ref dispatch_io for each ready descriptor. A
   * negative @p timeout_ms means block indefinitely.
   * @param timeout_ms Maximum wait in milliseconds, or negative for infinite.
   * @return Ok, or an Error on syscall failure.
   */
  virtual Status backend_poll(int timeout_ms) = 0;

 private:
  /** @brief A registered descriptor's interest and handler. */
  struct Registration {
    IoInterest interest;
    IoHandler* handler;
  };

  /** @brief A scheduled timer entry stored in the min-heap. */
  struct TimerEntry {
    TimerId id;
    Nanos deadline;
    Nanos interval;
    bool repeat;
    TimerHandler* handler;
  };

  /** @brief Min-heap ordering: earliest deadline has highest priority. */
  struct TimerGreater {
    bool operator()(const TimerEntry& a, const TimerEntry& b) const {
      return a.deadline > b.deadline;
    }
  };

  /**
   * @brief Fire all timers whose deadline is <= now; reschedule repeaters.
   */
  void fire_due_timers();

  /**
   * @brief Compute the poll timeout in ms given pending timers and a cap.
   * @param max_wait_ms Caller cap (-1 for none).
   * @return The effective timeout in milliseconds (>= 0, or -1 for infinite).
   */
  int compute_timeout_ms(int max_wait_ms) const;

  Clock* clock_;
  bool running_ = false;
  std::unordered_map<int, Registration> registrations_;

  std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerGreater>
      timers_;
  std::unordered_set<TimerId> cancelled_timers_;
  TimerId next_timer_id_ = 1;
};

/**
 * @brief Create the platform's default EventLoop backend.
 * @param clock Time source for timers (must outlive the loop).
 * @return A new loop (KqueueLoop or EpollLoop), or an Error on failure.
 */
Result<std::unique_ptr<EventLoop>> MakeEventLoop(Clock* clock);

}  // namespace codicis

#endif  // CODICIS_EVENT_EVENT_LOOP_H
