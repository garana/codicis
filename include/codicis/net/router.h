#ifndef CODICIS_NET_ROUTER_H
#define CODICIS_NET_ROUTER_H

/**
 * @file router.h
 * @brief Maps an HTTP method + path to a handler.
 *
 * A small exact-match router. Handlers are std::function (invoked once per
 * request, off the matching hot path), receiving the parsed request and a
 * response to populate.
 */

#include <functional>
#include <string>
#include <vector>

#include "codicis/net/http_message.h"

namespace codicis {

/**
 * @brief A request handler: reads @p req and fills @p resp.
 */
using HttpHandler =
    std::function<void(const HttpRequest& req, HttpResponse& resp)>;

/** @brief Result of attempting to route a request. */
enum class RouteResult {
  kHandled,           /**< A handler ran and populated the response. */
  kNotFound,          /**< No route matched the path. */
  kMethodNotAllowed,  /**< The path exists but not for this method. */
};

/**
 * @brief An exact-match method+path router.
 */
class HttpRouter {
 public:
  /**
   * @brief Register a handler for a method and path.
   * @param method  HTTP method (matched case-sensitively, e.g. "GET").
   * @param path    Exact request path (e.g. "/health").
   * @param handler The handler to invoke.
   */
  void add(std::string method, std::string path, HttpHandler handler);

  /**
   * @brief Route @p req, invoking a handler into @p resp on a match.
   * @param req  The parsed request.
   * @param resp The response to populate on a match.
   * @return Whether a handler ran, or why none did.
   */
  RouteResult route(const HttpRequest& req, HttpResponse& resp) const;

 private:
  /** @brief A registered route. */
  struct Entry {
    std::string method;
    std::string path;
    HttpHandler handler;
  };

  std::vector<Entry> routes_;
};

}  // namespace codicis

#endif  // CODICIS_NET_ROUTER_H
