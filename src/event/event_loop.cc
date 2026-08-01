/**
 * @file event_loop.cc
 * @brief Backend-independent EventLoop logic: bookkeeping and timers.
 */

#include "codicis/event/event_loop.h"

#include <algorithm>
#include <limits>

namespace codicis {

Status EventLoop::add(int fd, IoInterest interest, IoHandler* handler) {
  if (registrations_.find(fd) != registrations_.end()) {
    return Status(MakeError(ErrorCode::kConflict,
                            "fd already registered with the event loop"));
  }
  if (Status s = backend_add(fd, interest); !s.ok()) {
    return s;
  }
  registrations_[fd] = Registration{interest, handler};
  return Status::Ok();
}

Status EventLoop::modify(int fd, IoInterest interest) {
  const auto it = registrations_.find(fd);
  if (it == registrations_.end()) {
    return Status(MakeError(ErrorCode::kNotFound, "fd not registered"));
  }
  if (Status s = backend_modify(fd, interest); !s.ok()) {
    return s;
  }
  it->second.interest = interest;
  return Status::Ok();
}

Status EventLoop::remove(int fd) {
  const auto it = registrations_.find(fd);
  if (it == registrations_.end()) {
    return Status(MakeError(ErrorCode::kNotFound, "fd not registered"));
  }
  if (Status s = backend_remove(fd); !s.ok()) {
    return s;
  }
  registrations_.erase(it);
  return Status::Ok();
}

void EventLoop::dispatch_io(int fd, IoEvents events) {
  const auto it = registrations_.find(fd);
  if (it == registrations_.end()) {
    return;  // Removed during this poll cycle; drop the event.
  }
  it->second.handler->on_io_ready(fd, events);
}

TimerId EventLoop::add_timer(Nanos delay_ns, bool repeat,
                            TimerHandler* handler) {
  if (delay_ns < 0) {
    delay_ns = 0;
  }
  const TimerId id = next_timer_id_++;
  timers_.push(TimerEntry{id, clock_->now() + delay_ns, delay_ns, repeat,
                          handler});
  return id;
}

void EventLoop::cancel_timer(TimerId id) {
  cancelled_timers_.insert(id);
}

int EventLoop::compute_timeout_ms(int max_wait_ms) const {
  if (timers_.empty()) {
    return max_wait_ms;  // May be -1 (infinite).
  }
  const Nanos now = clock_->now();
  const Nanos deadline = timers_.top().deadline;
  if (deadline <= now) {
    return 0;
  }
  const Nanos remaining_ns = deadline - now;
  // Round up to the next whole millisecond so we never wake early.
  const Nanos ms64 = (remaining_ns + 999'999) / 1'000'000;
  int ms = (ms64 > std::numeric_limits<int>::max())
               ? std::numeric_limits<int>::max()
               : static_cast<int>(ms64);
  if (max_wait_ms >= 0) {
    ms = std::min(ms, max_wait_ms);
  }
  return ms;
}

void EventLoop::fire_due_timers() {
  const Nanos now = clock_->now();
  while (!timers_.empty() && timers_.top().deadline <= now) {
    TimerEntry entry = timers_.top();
    timers_.pop();

    const auto cancelled = cancelled_timers_.find(entry.id);
    if (cancelled != cancelled_timers_.end()) {
      cancelled_timers_.erase(cancelled);
      continue;
    }

    entry.handler->on_timer(entry.id);

    // The handler may have cancelled a repeating timer from within itself.
    if (entry.repeat) {
      const auto post = cancelled_timers_.find(entry.id);
      if (post != cancelled_timers_.end()) {
        cancelled_timers_.erase(post);
        continue;
      }
      entry.deadline = now + entry.interval;
      timers_.push(entry);
    }
  }
}

Status EventLoop::run_once(int max_wait_ms) {
  // Drop any cancelled timers sitting at the head so timeout math is correct.
  while (!timers_.empty()) {
    const auto it = cancelled_timers_.find(timers_.top().id);
    if (it == cancelled_timers_.end()) {
      break;
    }
    cancelled_timers_.erase(it);
    timers_.pop();
  }

  const int timeout_ms = compute_timeout_ms(max_wait_ms);
  Status s = backend_poll(timeout_ms);
  fire_due_timers();
  run_deferred();
  return s;
}

void EventLoop::run_deferred() {
  // Swap out so callbacks that themselves call defer() run next iteration.
  std::vector<std::function<void()>> batch;
  batch.swap(deferred_);
  for (auto& fn : batch) {
    fn();
  }
}

Status EventLoop::run() {
  running_ = true;
  while (running_) {
    if (Status s = run_once(-1); !s.ok()) {
      running_ = false;
      return s;
    }
  }
  return Status::Ok();
}

}  // namespace codicis
