#ifndef CODICIS_IPC_HELPER_CLIENT_H
#define CODICIS_IPC_HELPER_CLIENT_H

/**
 * @file helper_client.h
 * @brief Event-loop client for a child-process helper over pipes.
 *
 * HelperClient speaks a @ref codicis::HelperCodec to a helper across a read
 * and a write descriptor (a socketpair, or a child's stdout/stdin). Requests
 * are **pipelined**: several may be in flight and responses may return out of
 * order; each is correlated by a monotonically increasing req_id. This is the
 * generic transport shared by all helper types (storage, trade reporter,
 * last-price reporter, ...).
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codicis/event/event_loop.h"
#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/helper_message.h"
#include "codicis/util/buffer.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief Response callback.
 * @param ok    True if @p reply is a real response; false if the helper closed
 *              before replying.
 * @param reply The response message (valid only when @p ok is true).
 */
using ResponseFn =
    std::function<void(bool ok, const HelperMessage& reply)>;

/**
 * @brief Pipelined client for one helper connection.
 */
class HelperClient final : public IoHandler, public TimerHandler {
 public:
  /**
   * @brief Construct over a read and a write descriptor.
   *
   * The descriptors may be equal (a socketpair) or distinct (pipe ends). Both
   * must be non-blocking. The client registers @p read_fd with the loop.
   * @param loop                The event loop (must outlive this).
   * @param read_fd             Descriptor to read helper responses from.
   * @param write_fd            Descriptor to write helper requests to.
   * @param codec               The wire codec (must outlive this).
   * @param request_timeout_ns  Fail a request whose response does not arrive
   *                            within this many nanoseconds; 0 disables
   *                            timeouts.
   */
  HelperClient(EventLoop& loop, int read_fd, int write_fd,
               const HelperCodec& codec, Nanos request_timeout_ns = 0);

  ~HelperClient() override;

  HelperClient(const HelperClient&) = delete;
  HelperClient& operator=(const HelperClient&) = delete;

  /**
   * @brief Send a request and register a response callback.
   *
   * If the connection is already closed, @p cb is invoked immediately with
   * ok=false rather than being silently dropped.
   * @param type   The message type.
   * @param fields The request fields.
   * @param cb     Response callback (may be empty for fire-and-forget).
   * @return The assigned req_id.
   */
  std::uint64_t send(std::string type,
                     std::vector<std::pair<std::string, std::string>> fields,
                     ResponseFn cb);

  void on_io_ready(int fd, IoEvents events) override;
  void on_timer(TimerId id) override;

  /** @return The number of requests awaiting a response. */
  std::size_t pending_count() const { return pending_.size(); }

  /** @return True once the helper connection has closed. */
  bool closed() const { return closed_; }

  /**
   * @brief Record the child PID so the destructor can reap it.
   * @param pid The child process id.
   */
  void set_child_pid(int pid) { child_pid_ = pid; }

 private:
  /** @brief An in-flight request: its callback and (optional) deadline. */
  struct Pending {
    ResponseFn cb;
    Nanos deadline;  // 0 if timeouts are disabled
  };

  void on_readable();
  void on_writable();
  void set_write_interest(bool on);
  void handle_close();

  EventLoop& loop_;
  int read_fd_;
  int write_fd_;
  bool same_fd_;
  const HelperCodec& codec_;
  Nanos timeout_ns_;

  Buffer in_;
  Buffer out_;
  std::unordered_map<std::uint64_t, Pending> pending_;
  std::uint64_t next_req_id_ = 1;
  TimerId sweep_timer_ = 0;

  bool write_enabled_ = false;   /**< Same-fd write interest state. */
  bool write_registered_ = false; /**< Distinct-fd write registration state. */
  bool closed_ = false;
  int child_pid_ = -1;
};

/**
 * @brief Spawn a child-process helper and connect a HelperClient to it.
 *
 * The child's stdin receives requests and its stdout yields responses; stderr
 * is inherited for logging. SIGPIPE is ignored process-wide.
 * @param loop                The event loop (must outlive the client).
 * @param argv                The command and arguments (argv[0] is the program).
 * @param codec               The wire codec (must outlive the client).
 * @param request_timeout_ns  Per-request timeout in nanoseconds (0 disables).
 * @return The connected client, or an Error on pipe/fork/exec failure.
 */
Result<std::unique_ptr<HelperClient>> SpawnHelper(
    EventLoop& loop, const std::vector<std::string>& argv,
    const HelperCodec& codec, Nanos request_timeout_ns = 0);

}  // namespace codicis

#endif  // CODICIS_IPC_HELPER_CLIENT_H
