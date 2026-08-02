#ifndef CODICIS_APP_SERVER_H
#define CODICIS_APP_SERVER_H

/**
 * @file server.h
 * @brief The application server: wires HTTP -> matching engine -> storage.
 *
 * AppServer owns the order book, an HTTP server, and a storage helper client,
 * all driven by one event loop. REST endpoints submit and cancel orders and
 * report the book; matched orders and trades are reported to the storage
 * helper, which is committed periodically.
 *
 * Note: the strict spec ordering (report to storage, await confirmation, then
 * place in memory) and async HTTP responses are deferred to the storage-
 * determinism phase (OT8). Here matching is synchronous and storage reporting
 * is asynchronous/best-effort with periodic commit.
 */

#include <cstdint>
#include <memory>

#include "codicis/config/config.h"
#include "codicis/core/order_book.h"
#include "codicis/engine/trading_engine.h"
#include "codicis/event/event_loop.h"
#include "codicis/ipc/helper_client.h"
#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/storage_client.h"
#include "codicis/net/http_server.h"
#include "codicis/net/router.h"
#include "codicis/util/result.h"

namespace codicis {

/**
 * @brief Ties together networking, matching, and storage on one event loop.
 */
class AppServer : public TimerHandler {
 public:
  /**
   * @brief Construct the server.
   * @param loop   The event loop (must outlive the server).
   * @param config Resolved configuration (must outlive the server).
   */
  AppServer(EventLoop& loop, const Config& config);

  ~AppServer() override;

  /**
   * @brief Spawn the storage helper and begin listening for HTTP.
   * @return Ok, or an Error on startup failure.
   */
  Status start();

  /** @return The bound HTTP port (valid after a successful @ref start). */
  std::uint16_t http_port() const;

  void on_timer(TimerId id) override;

 private:
  void setup_routes();
  void handle_submit(const HttpRequest& req, HttpResponder respond);
  void handle_cancel(const HttpRequest& req, HttpResponse& resp);
  void handle_book(const HttpRequest& req, HttpResponse& resp);

  EventLoop& loop_;
  const Config& config_;

  OrderBook book_;
  HttpRouter router_;
  std::unique_ptr<HelperCodec> codec_;
  std::unique_ptr<HelperClient> helper_;
  std::unique_ptr<StorageClient> storage_;
  std::unique_ptr<TradingEngine> engine_;
  std::unique_ptr<HttpServer> http_;

  TimerId commit_timer_ = 0;
};

}  // namespace codicis

#endif  // CODICIS_APP_SERVER_H
