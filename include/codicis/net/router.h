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
 * @brief A synchronous request handler: reads @p req and fills @p resp.
 */
using HttpHandler =
    std::function<void(const HttpRequest& req, HttpResponse& resp)>;

/**
 * @brief A completion used to deliver a response, possibly later.
 * @param resp The response to send.
 */
using HttpResponder = std::function<void(HttpResponse resp)>;

/**
 * @brief An asynchronous handler: responds now or later via @p respond.
 *
 * The handler must extract everything it needs from @p req synchronously (the
 * reference is not valid after it returns), then call @p respond exactly once.
 */
using HttpAsyncHandler =
    std::function<void(const HttpRequest& req, HttpResponder respond)>;

/** @brief Result of attempting to route a request. */
enum class RouteResult {
  kHandled,           /**< A handler ran and populated the response. */
  kNotFound,          /**< No route matched the path. */
  kMethodNotAllowed,  /**< The path exists but not for this method. */
};

/**
 * @brief An exact-match method+path router supporting sync and async handlers.
 */
class HttpRouter {
 public:
  /**
   * @brief Register a synchronous handler for a method and path.
   * @param method  HTTP method (matched case-sensitively, e.g. "GET").
   * @param path    Exact request path (e.g. "/health").
   * @param handler The handler to invoke.
   */
  void add(std::string method, std::string path, HttpHandler handler);

  /**
   * @brief Register an asynchronous handler for a method and path.
   * @param method  HTTP method.
   * @param path    Exact request path.
   * @param handler The async handler to invoke.
   */
  void add_async(std::string method, std::string path,
                 HttpAsyncHandler handler);

  /**
   * @brief Dispatch @p req, delivering the response through @p respond.
   *
   * Sync handlers (and the 404/405 fallbacks) call @p respond before returning;
   * async handlers may call it later. @p respond is always invoked exactly
   * once.
   * @param req     The parsed request.
   * @param respond The completion used to deliver the response.
   */
  void dispatch(const HttpRequest& req, HttpResponder respond) const;

 private:
  /** @brief A registered route (exactly one handler is set). */
  struct Entry {
    std::string method;
    std::string path;
    HttpHandler sync;
    HttpAsyncHandler async;
  };

  std::vector<Entry> routes_;
};

}  // namespace codicis

#endif  // CODICIS_NET_ROUTER_H
