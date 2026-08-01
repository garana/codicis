#ifndef CODICIS_NET_LISTENER_H
#define CODICIS_NET_LISTENER_H

/**
 * @file listener.h
 * @brief A non-blocking TCP listener driven by the event loop.
 *
 * On readiness the listener accepts all pending connections and hands each
 * accepted (non-blocking) descriptor to a callback. It does not own accepted
 * connections; higher layers (e.g. HttpServer) do.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "codicis/event/event_loop.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief A TCP listening socket registered with an event loop.
 */
class TcpListener final : public IoHandler {
 public:
  /** @brief Called with each accepted, non-blocking descriptor. */
  using AcceptFn = std::function<void(int fd)>;

  /**
   * @brief Create and register a listener.
   * @param loop      The event loop to register with (must outlive this).
   * @param addr      Dotted IPv4 bind address.
   * @param port      Bind port; 0 selects an ephemeral port.
   * @param on_accept Callback invoked per accepted connection.
   * @return The listener, or an Error on socket setup failure.
   */
  static Result<std::unique_ptr<TcpListener>> Create(EventLoop& loop,
                                                     const std::string& addr,
                                                     std::uint16_t port,
                                                     AcceptFn on_accept);

  ~TcpListener() override;

  /** @return The actual bound port (useful when 0 was requested). */
  std::uint16_t port() const { return port_; }

  void on_io_ready(int fd, IoEvents events) override;

 private:
  TcpListener(EventLoop& loop, int fd, std::uint16_t port, AcceptFn on_accept)
      : loop_(loop), fd_(fd), port_(port), on_accept_(std::move(on_accept)) {}

  EventLoop& loop_;
  int fd_;
  std::uint16_t port_;
  AcceptFn on_accept_;
};

}  // namespace codicis

#endif  // CODICIS_NET_LISTENER_H
