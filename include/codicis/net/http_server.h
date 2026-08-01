#ifndef CODICIS_NET_HTTP_SERVER_H
#define CODICIS_NET_HTTP_SERVER_H

/**
 * @file http_server.h
 * @brief A minimal HTTP/1.1 server on the event loop.
 *
 * HttpServer owns a @ref TcpListener and the set of live connections. Each
 * connection parses requests with @ref HttpRequestParser, routes them through
 * an @ref HttpRouter, and writes responses back, all non-blocking and driven
 * by the shared @ref EventLoop. Keep-alive and pipelining are supported.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "codicis/event/event_loop.h"
#include "codicis/net/listener.h"
#include "codicis/net/router.h"
#include "codicis/util/result.h"

namespace codicis {

class HttpConnection;  // internal, defined in http_server.cc

/**
 * @brief An event-loop-driven HTTP/1.1 server.
 */
class HttpServer {
 public:
  /**
   * @brief Construct a server.
   * @param loop   The event loop (must outlive the server).
   * @param router The route table (must outlive the server).
   */
  HttpServer(EventLoop& loop, const HttpRouter& router);

  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  /**
   * @brief Begin listening for connections.
   * @param addr Dotted IPv4 bind address.
   * @param port Bind port; 0 selects an ephemeral port.
   * @return Ok, or an Error on socket setup failure.
   */
  Status listen(const std::string& addr, std::uint16_t port);

  /** @return The bound port (valid after a successful @ref listen). */
  std::uint16_t port() const;

  /** @return The number of currently open connections. */
  std::size_t connection_count() const { return conns_.size(); }

 private:
  /** @brief Accept callback: wrap @p fd in a connection. */
  void on_accept(int fd);

  /** @brief Tear down the connection for @p fd (deferred, dispatch-safe). */
  void close_connection(int fd);

  EventLoop& loop_;
  const HttpRouter& router_;
  std::unique_ptr<TcpListener> listener_;
  std::unordered_map<int, std::unique_ptr<HttpConnection>> conns_;
};

}  // namespace codicis

#endif  // CODICIS_NET_HTTP_SERVER_H
