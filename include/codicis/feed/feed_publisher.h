#ifndef CODICIS_FEED_FEED_PUBLISHER_H
#define CODICIS_FEED_FEED_PUBLISHER_H

/**
 * @file feed_publisher.h
 * @brief Publishes the book-event stream to a spawned feed-helper child.
 *
 * FeedPublisher is the codicis-side @ref BookEventSink: the MatchingEngine calls
 * it for every book mutation and it serializes each event (feed wire codec) into
 * a bounded outbound buffer drained to the feed-helper's stdin on the event
 * loop. The feed path is deliberately best-effort and NON-blocking: if the
 * helper falls behind and the buffer hits its cap, events are DROPPED (counted)
 * rather than stalling the matcher -- the resulting @c seq gap is what tells the
 * helper's replica to resync. This is the opposite of the reliable, acked
 * storage path.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "codicis/core/book_event.h"
#include "codicis/event/event_loop.h"
#include "codicis/util/buffer.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief Best-effort publisher of book events to a child feed-helper over a
 *        pipe, drained on the event loop.
 */
class FeedPublisher final : public BookEventSink, public IoHandler {
 public:
  /**
   * @brief Construct over a write descriptor (the helper's stdin).
   * @param loop     The event loop (must outlive this).
   * @param write_fd Non-blocking descriptor to write encoded events to; owned.
   */
  FeedPublisher(EventLoop& loop, int write_fd);

  ~FeedPublisher() override;

  FeedPublisher(const FeedPublisher&) = delete;
  FeedPublisher& operator=(const FeedPublisher&) = delete;

  /** @brief BookEventSink: encode @p ev and queue it (drop if over cap). */
  void on_book_event(const BookEvent& ev) override;

  void on_io_ready(int fd, IoEvents events) override;

  /** @brief Record the child PID so the destructor can reap it. */
  void set_child_pid(int pid) { child_pid_ = pid; }

  /** @return The number of events dropped due to a full outbound buffer. */
  std::uint64_t dropped() const { return dropped_; }

 private:
  void flush();
  void arm_write(bool on);
  void handle_close();

  EventLoop& loop_;
  int write_fd_;
  Buffer out_;
  bool write_armed_ = false;
  bool closed_ = false;
  int child_pid_ = -1;
  std::uint64_t dropped_ = 0;
};

/**
 * @brief Spawn the feed-helper child and connect a FeedPublisher to its stdin.
 *
 * The child's stdout/stderr are inherited (its "listening" banner and logs go
 * to the operator). codicis only writes the event stream to the child's stdin.
 * @param loop The event loop (must outlive the returned publisher).
 * @param argv The command and arguments (argv[0] is the program).
 * @return The connected publisher, or an Error on pipe/fork/exec failure.
 */
Result<std::unique_ptr<FeedPublisher>> SpawnFeedHelper(
    EventLoop& loop, const std::vector<std::string>& argv);

}  // namespace codicis

#endif  // CODICIS_FEED_FEED_PUBLISHER_H
