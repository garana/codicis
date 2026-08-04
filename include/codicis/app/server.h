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
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "codicis/auth/auth_client.h"
#include "codicis/config/config.h"
#include "codicis/core/matching_engine.h"
#include "codicis/core/order_book.h"
#include "codicis/engine/trading_engine.h"
#include "codicis/event/event_loop.h"
#include "codicis/util/clock.h"
#include "codicis/ipc/helper_client.h"
#include "codicis/ipc/helper_codec.h"
#include "codicis/ipc/storage_client.h"
#include "codicis/net/http_server.h"
#include "codicis/net/router.h"
#include "codicis/net/websocket.h"
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

  /** @return The bound WebSocket port (valid after a successful @ref start). */
  std::uint16_t ws_port() const;

  /** @return The bound aggregator WS port (0 if not enabled). */
  std::uint16_t aggregator_ws_port() const;

  void on_timer(TimerId id) override;

 private:
  void setup_routes();
  void handle_submit(const HttpRequest& req, const HttpResponder& respond);
  void handle_cancel(const HttpRequest& req, const HttpResponder& respond);
  void handle_book(const HttpRequest& req, HttpResponse& resp);

  /** @brief Run a symbol's opening/closing auction and report the prints. */
  void handle_auction(const HttpRequest& req, HttpResponse& resp);

  /** @brief Submit an order received over a WebSocket and stream the result. */
  void handle_ws_message(WsConnection& conn, bool is_binary,
                         std::string_view payload);

  /** @brief Resolve the connection's owner identity from the handshake. */
  void on_ws_open(WsConnection& conn, const HttpRequest& handshake);

  /**
   * @brief Result of authenticating a request.
   * @param ok     True if the request carries a trusted identity.
   * @param status HTTP status to return when @p ok is false (401/403).
   * @param owner  The authenticated owner UUID ("" when auth is disabled).
   * @param err    A human-readable reason when @p ok is false.
   */
  using AuthFn = std::function<void(bool ok, int status, std::string owner,
                                    std::string err)>;

  /**
   * @brief Authenticate a request per the configured mechanisms.
   *
   * Reads the identity from the trusted header (Option A) and/or validates a
   * credential header via the auth helper (Option B). When both are enabled
   * they must both pass and resolve to the same UUID. Invokes @p cb (possibly
   * asynchronously, when the helper is consulted). Anonymous ("" owner) when no
   * mechanism is enabled. The request's headers are read synchronously, so the
   * request need not outlive the call.
   */
  void authenticate(const HttpRequest& req, AuthFn cb);

  /**
   * @brief Push a market-data update (top of book + trades) to a symbol's
   *        subscribers.
   * @param symbol The instrument whose book changed.
   * @param trades Trades that just occurred (may be empty for a book change).
   */
  void broadcast_market_data(const Symbol& symbol,
                             const std::vector<Trade>& trades);

  EventLoop& loop_;
  const Config& config_;

  std::size_t mem_levels_ = 0;  // resident-window bound (0 = unbounded)
  std::size_t outbox_max_ = 0;  // un-committed reports before 503 (0=unbounded)
  MatchingEngine matching_;
  HttpRouter router_;
  std::unique_ptr<HelperCodec> codec_;
  std::unique_ptr<HelperClient> helper_;
  std::unique_ptr<StorageClient> storage_;
  std::unique_ptr<TradingEngine> engine_;
  std::unique_ptr<HttpServer> http_;
  std::unique_ptr<WsServer> ws_;

  // Authentication (identity of the requesting user).
  bool auth_header_enabled_ = false;
  std::string auth_header_name_;
  bool auth_helper_enabled_ = false;
  std::string auth_credential_header_;
  WallClock auth_clock_;  // wall time for AuthClient cache expiry
  std::unique_ptr<AuthClient> auth_;

  // Per-connection authenticated owner (present once a WS handshake resolves).
  std::unordered_map<std::uint64_t, std::string> conn_owner_;
  // Aggregator connections (internal port, unauthenticated handshake) that
  // supply a per-frame `user` uuid instead of a single connection identity.
  std::unordered_set<std::uint64_t> conn_perframe_;

  // Market-data subscribers per symbol: symbol -> (connection id -> fd).
  std::unordered_map<Symbol, std::unordered_map<std::uint64_t, int>>
      md_subscribers_;

  TimerId commit_timer_ = 0;
};

}  // namespace codicis

#endif  // CODICIS_APP_SERVER_H
