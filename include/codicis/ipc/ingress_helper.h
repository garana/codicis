#ifndef CODICIS_IPC_INGRESS_HELPER_H
#define CODICIS_IPC_INGRESS_HELPER_H

/**
 * @file ingress_helper.h
 * @brief Server side of a child-process helper that *initiates* requests.
 *
 * The storage/auth helpers are driven by codicis (codicis sends a request, the
 * helper replies). An **ingress** helper is the mirror image: it is the source
 * of client-side traffic (orders, cancels) that it pulls from some external
 * system (Kafka, RabbitMQ, SQS, ...) and pushes into codicis. It speaks the
 * same @ref codicis::HelperCodec, but here the helper writes requests to its
 * stdout (codicis reads them) and codicis writes replies to the helper's stdin.
 *
 * Because one ingress helper multiplexes many end-users, every request carries
 * its own `user` identity per message (as with the aggregator WebSocket). Trust
 * comes from the helper being a spawned, managed child over a private pipe.
 */

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "codicis/event/event_loop.h"
#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/helper_message.h"
#include "codicis/util/buffer.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief Reads helper-initiated requests and writes correlated replies, on the
 *        event loop.
 */
class IngressHelper final : public IoHandler {
 public:
  /**
   * @brief Sink for a request's reply. The req_id is stamped automatically to
   * match the request, so a handler fills only @c type and @c fields. Safe to
   * call later (asynchronously); a no-op once the helper has closed.
   */
  using Reply = std::function<void(HelperMessage)>;

  /**
   * @brief Handle one request the helper initiated.
   * @param req   The decoded request message.
   * @param reply Call exactly once (now or later) with the response.
   */
  using DispatchFn = std::function<void(const HelperMessage& req, Reply reply)>;

  /**
   * @brief Construct over a read and a write descriptor.
   * @param loop     The event loop (must outlive this).
   * @param read_fd  Descriptor to read helper-initiated requests from.
   * @param write_fd Descriptor to write replies to.
   * @param codec    The wire codec (must outlive this).
   * @param dispatch Invoked for each decoded request.
   */
  IngressHelper(EventLoop& loop, int read_fd, int write_fd,
                const HelperCodec& codec, DispatchFn dispatch);

  ~IngressHelper() override;

  IngressHelper(const IngressHelper&) = delete;
  IngressHelper& operator=(const IngressHelper&) = delete;

  void on_io_ready(int fd, IoEvents events) override;

  /** @return True once the helper connection has closed. */
  bool closed() const { return closed_; }

  /** @brief Record the child PID so the destructor can reap it. */
  void set_child_pid(int pid) { child_pid_ = pid; }

 private:
  void on_readable();
  void on_writable();
  void set_write_interest(bool on);
  void handle_close();
  void queue_reply(HelperMessage msg);

  EventLoop& loop_;
  int read_fd_;
  int write_fd_;
  bool same_fd_;
  const HelperCodec& codec_;
  DispatchFn dispatch_;

  Buffer in_;
  Buffer out_;

  bool write_enabled_ = false;    /**< Same-fd write interest state. */
  bool write_registered_ = false; /**< Distinct-fd write registration state. */
  bool closed_ = false;
  int child_pid_ = -1;
};

/**
 * @brief Spawn a child-process ingress helper and wire it to the loop.
 *
 * The child's stdout carries requests (codicis reads) and its stdin carries
 * replies (codicis writes); stderr is inherited for logging.
 * @param loop     The event loop (must outlive the returned object).
 * @param argv     The command and arguments (argv[0] is the program).
 * @param codec    The wire codec (must outlive the returned object).
 * @param dispatch Invoked for each request the helper initiates.
 * @return The connected ingress helper, or an Error on pipe/fork/exec failure.
 */
Result<std::unique_ptr<IngressHelper>> SpawnIngressHelper(
    EventLoop& loop, const std::vector<std::string>& argv,
    const HelperCodec& codec, IngressHelper::DispatchFn dispatch);

}  // namespace codicis

#endif  // CODICIS_IPC_INGRESS_HELPER_H
