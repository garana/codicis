/**
 * @file router.cc
 * @brief Implementation of HttpRouter (see router.h).
 */

#include "codicis/net/router.h"

#include <utility>

namespace codicis {

void HttpRouter::add(std::string method, std::string path,
                     HttpHandler handler) {
  routes_.push_back(
      Entry{std::move(method), std::move(path), std::move(handler)});
}

RouteResult HttpRouter::route(const HttpRequest& req,
                              HttpResponse& resp) const {
  bool path_matched = false;
  for (const Entry& e : routes_) {
    if (e.path != req.path) {
      continue;
    }
    path_matched = true;
    if (e.method == req.method) {
      e.handler(req, resp);
      return RouteResult::kHandled;
    }
  }
  return path_matched ? RouteResult::kMethodNotAllowed
                      : RouteResult::kNotFound;
}

}  // namespace codicis
