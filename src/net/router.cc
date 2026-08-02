/**
 * @file router.cc
 * @brief Implementation of HttpRouter (see router.h).
 */

#include "codicis/net/router.h"

#include <utility>

namespace codicis {

void HttpRouter::add(std::string method, std::string path,
                     HttpHandler handler) {
  routes_.push_back(Entry{std::move(method), std::move(path),
                          std::move(handler), nullptr});
}

void HttpRouter::add_async(std::string method, std::string path,
                           HttpAsyncHandler handler) {
  routes_.push_back(Entry{std::move(method), std::move(path), nullptr,
                          std::move(handler)});
}

void HttpRouter::dispatch(const HttpRequest& req,
                          HttpResponder respond) const {
  bool path_matched = false;
  for (const Entry& e : routes_) {
    if (e.path != req.path) {
      continue;
    }
    path_matched = true;
    if (e.method == req.method) {
      if (e.async) {
        e.async(req, std::move(respond));
      } else {
        HttpResponse resp;
        e.sync(req, resp);
        respond(std::move(resp));
      }
      return;
    }
  }

  HttpResponse resp;
  if (path_matched) {
    resp.set_status(405);
    resp.set_header("Content-Type", "text/plain");
    resp.body = "Method Not Allowed";
  } else {
    resp.set_status(404);
    resp.set_header("Content-Type", "text/plain");
    resp.body = "Not Found";
  }
  respond(std::move(resp));
}

}  // namespace codicis
