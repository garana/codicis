/**
 * @file server.cc
 * @brief Implementation of AppServer (see server.h).
 */

#include "codicis/app/server.h"

#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codicis/core/order.h"
#include "codicis/util/logging.h"
#include "codicis/util/uuid.h"

namespace codicis {
namespace {

/** @brief Split a string on a delimiter. */
std::vector<std::string> Split(std::string_view s, char delim) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos <= s.size()) {
    const std::size_t next = s.find(delim, pos);
    const std::size_t end = (next == std::string_view::npos) ? s.size() : next;
    if (end > pos) {
      out.emplace_back(s.substr(pos, end - pos));
    }
    if (next == std::string_view::npos) {
      break;
    }
    pos = next + 1;
  }
  return out;
}

/** @brief Parse an application/x-www-form-urlencoded body (no %-decoding). */
std::vector<std::pair<std::string, std::string>> ParseForm(
    std::string_view body) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const std::string& pair : Split(body, '&')) {
    const std::size_t eq = pair.find('=');
    if (eq == std::string::npos) {
      out.emplace_back(pair, "");
    } else {
      out.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
    }
  }
  return out;
}

/** @brief Look up a form field. */
const std::string* FormGet(
    const std::vector<std::pair<std::string, std::string>>& form,
    std::string_view key) {
  for (const auto& kv : form) {
    if (kv.first == key) {
      return &kv.second;
    }
  }
  return nullptr;
}

/** @brief Parse a base-10 integer; returns false on any invalid character. */
bool ParseI64(std::string_view s, std::int64_t* out) {
  if (s.empty()) {
    return false;
  }
  std::int64_t sign = 1;
  std::size_t i = 0;
  if (s[0] == '-') {
    sign = -1;
    i = 1;
  }
  if (i == s.size()) {
    return false;
  }
  std::int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
    v = v * 10 + (s[i] - '0');
  }
  *out = v * sign;
  return true;
}

/** @brief Parse a side name. */
bool ParseSide(std::string_view s, Side* out) {
  if (s == "buy") {
    *out = Side::Buy;
    return true;
  }
  if (s == "sell") {
    *out = Side::Sell;
    return true;
  }
  return false;
}

/** @brief Parse an order type name. */
bool ParseType(std::string_view s, OrdType* out) {
  if (s == "limit") {
    *out = OrdType::Limit;
    return true;
  }
  if (s == "market") {
    *out = OrdType::Market;
    return true;
  }
  return false;
}

/** @brief Parse a time-in-force name (default GTC). */
bool ParseTif(std::string_view s, Tif* out) {
  if (s == "gtc") { *out = Tif::GTC; return true; }
  if (s == "day") { *out = Tif::DAY; return true; }
  if (s == "gtd") { *out = Tif::GTD; return true; }
  if (s == "ioc") { *out = Tif::IOC; return true; }
  if (s == "fok") { *out = Tif::FOK; return true; }
  if (s == "gtx") { *out = Tif::GTX; return true; }
  return false;
}

/** @brief A JSON error object as a string. */
std::string JsonErrorBody(const std::string& message) {
  return "{\"error\":\"" + message + "\"}";
}

/** @brief Reply with a JSON error object. */
void JsonError(HttpResponse& resp, int status, const std::string& message) {
  resp.set_status(status);
  resp.set_header("Content-Type", "application/json");
  resp.body = JsonErrorBody(message);
}

/**
 * @brief Build an Order from a parsed form (transport-agnostic).
 * @param form The parsed key/value fields.
 * @param out  Receives the order (id left 0 for the engine to assign).
 * @param err  Receives a message on failure.
 * @return True on success.
 */
bool ParseOrderForm(
    const std::vector<std::pair<std::string, std::string>>& form,
    Symbol* symbol, Order* out, std::string* err) {
  const std::string* symbol_s = FormGet(form, "symbol");
  const std::string* side_s = FormGet(form, "side");
  const std::string* type_s = FormGet(form, "type");
  const std::string* qty_s = FormGet(form, "qty");
  Side side;
  OrdType type;
  std::int64_t qty = 0;
  if (symbol_s == nullptr || symbol_s->empty()) {
    *err = "missing symbol";
    return false;
  }
  if (side_s == nullptr || !ParseSide(*side_s, &side)) {
    *err = "missing or invalid side";
    return false;
  }
  if (type_s == nullptr || !ParseType(*type_s, &type)) {
    *err = "missing or invalid type";
    return false;
  }
  if (qty_s == nullptr || !ParseI64(*qty_s, &qty) || qty <= 0) {
    *err = "missing or invalid qty";
    return false;
  }

  Order order;
  order.side = side;
  order.type = type;
  order.qty = qty;
  if (type == OrdType::Limit) {
    const std::string* price_s = FormGet(form, "price");
    std::int64_t price = 0;
    if (price_s == nullptr || !ParseI64(*price_s, &price) || price <= 0) {
      *err = "missing or invalid price";
      return false;
    }
    order.price = price;
    order.tif = Tif::GTC;
  } else {
    order.tif = Tif::IOC;  // market orders never rest
  }
  if (const std::string* tif_s = FormGet(form, "tif")) {
    if (!ParseTif(*tif_s, &order.tif)) {
      *err = "invalid tif";
      return false;
    }
  }
  *symbol = *symbol_s;
  *out = order;
  return true;
}

/** @brief JSON body describing a successful submit outcome. */
std::string SubmitBody(const std::string& order_uuid, const SubmitOutcome& out) {
  std::ostringstream body;
  body << "{\"accepted\":true,\"order\":\"" << order_uuid
       << "\",\"order_id\":" << out.order_id << ",\"filled\":" << out.filled
       << ",\"rested\":" << (out.rested ? "true" : "false") << ",\"trades\":[";
  for (std::size_t i = 0; i < out.trades.size(); ++i) {
    const Trade& t = out.trades[i];
    if (i != 0) {
      body << ",";
    }
    body << "{\"price\":" << t.price << ",\"qty\":" << t.qty
         << ",\"maker\":" << t.maker_id << "}";
  }
  body << "]}";
  return body.str();
}

/** @brief JSON body for a completed submit (success or failure). */
std::string ResultBody(const TradingEngine::Result& res) {
  if (!res.storage_ok) {
    return JsonErrorBody("storage unavailable");
  }
  if (!res.outcome.accepted) {
    return JsonErrorBody(res.outcome.reject_reason);
  }
  return SubmitBody(res.order_uuid, res.outcome);
}

/** @brief Top-of-book JSON fields for a symbol: "bid",... ,"ask",... . */
std::string BookTopFields(const MatchingEngine& engine, const Symbol& symbol) {
  std::ostringstream f;
  f << "\"symbol\":\"" << symbol << "\",";
  Ticks bid = 0;
  Ticks ask = 0;
  if (engine.best_bid(symbol, &bid)) {
    f << "\"bid\":" << bid
      << ",\"bid_qty\":" << engine.total_qty_at(symbol, Side::Buy, bid);
  } else {
    f << "\"bid\":null,\"bid_qty\":0";
  }
  f << ",";
  if (engine.best_ask(symbol, &ask)) {
    f << "\"ask\":" << ask
      << ",\"ask_qty\":" << engine.total_qty_at(symbol, Side::Sell, ask);
  } else {
    f << "\"ask\":null,\"ask_qty\":0";
  }
  return f.str();
}

}  // namespace

AppServer::AppServer(EventLoop& loop, const Config& config)
    : loop_(loop), config_(config) {}

AppServer::~AppServer() {
  if (commit_timer_ != 0) {
    loop_.cancel_timer(commit_timer_);
  }
}

Status AppServer::start() {
  const std::string codec_name = config_.get_string("storage.codec").value();
  if (codec_name == "binary") {
    codec_ = std::make_unique<BinaryHelperCodec>();
  } else {
    codec_ = std::make_unique<TextHelperCodec>();
  }

  const std::string cmd = config_.get_string("storage.helper_cmd").value();
  std::vector<std::string> argv = Split(cmd, ' ');
  if (argv.empty()) {
    return Status(MakeError(ErrorCode::kInvalidArg, "empty storage.helper_cmd"));
  }
  const std::int64_t timeout_ms =
      config_.get_int("storage.request_timeout_ms").value();
  Result<std::unique_ptr<HelperClient>> hr =
      SpawnHelper(loop_, argv, *codec_, timeout_ms * 1'000'000);
  if (!hr.ok()) {
    return hr.error();
  }
  helper_ = std::move(hr.value());
  storage_ = std::make_unique<StorageClient>(*helper_);
  engine_ = std::make_unique<TradingEngine>(matching_, *storage_);

  // Authentication: Option A (trusted header) and/or Option B (helper pool).
  auth_header_enabled_ = config_.get_bool("auth.header.enabled").value();
  auth_header_name_ = config_.get_string("auth.header.name").value();
  auth_helper_enabled_ = config_.get_bool("auth.helper.enabled").value();
  auth_credential_header_ =
      config_.get_string("auth.helper.credential_header").value();
  if (auth_helper_enabled_) {
    AuthClient::Config ac;
    ac.argv = Split(config_.get_string("auth.helper.cmd").value(), ' ');
    if (ac.argv.empty()) {
      return Status(MakeError(ErrorCode::kInvalidArg, "empty auth.helper.cmd"));
    }
    ac.concurrency = static_cast<std::size_t>(
        config_.get_int("auth.helper.concurrency").value());
    const bool pipelining = config_.get_bool("auth.helper.pipelining").value();
    const std::int64_t depth =
        config_.get_int("auth.helper.pipeline_depth").value();
    ac.depth = pipelining ? static_cast<std::size_t>(depth) : 1;
    ac.request_timeout_ns =
        config_.get_int("auth.helper.request_timeout_ms").value() * 1'000'000;
    ac.positive_capacity = static_cast<std::size_t>(
        config_.get_int("auth.cache.max_entries").value());
    ac.positive_max_bytes = static_cast<std::size_t>(
        config_.get_int("auth.cache.max_bytes").value());
    ac.positive_ttl_ns =
        config_.get_int("auth.cache.ttl_ms").value() * 1'000'000;
    ac.negative_capacity = static_cast<std::size_t>(
        config_.get_int("auth.cache.negative_max_entries").value());
    ac.negative_max_bytes = static_cast<std::size_t>(
        config_.get_int("auth.cache.negative_max_bytes").value());
    ac.negative_ttl_ns =
        config_.get_int("auth.cache.negative_ttl_ms").value() * 1'000'000;
    ac.max_purge_per_insert = static_cast<std::size_t>(
        config_.get_int("auth.cache.max_purge_per_insert").value());
    Result<std::unique_ptr<AuthClient>> ar =
        AuthClient::Create(loop_, auth_clock_, std::move(ac));
    if (!ar.ok()) {
      return ar.error();
    }
    auth_ = std::move(ar.value());
  }

  setup_routes();
  const std::string addr = config_.get_string("net.bind_address").value();

  http_ = std::make_unique<HttpServer>(loop_, router_);
  const auto http_port_cfg = static_cast<std::uint16_t>(
      config_.get_int("net.http_port").value());
  if (Status s = http_->listen(addr, http_port_cfg); !s.ok()) {
    return s;
  }

  // WebSocket endpoint: clients submit orders and/or subscribe to market data.
  ws_ = std::make_unique<WsServer>(
      loop_,
      [this](WsConnection& conn, bool is_binary, std::string_view payload) {
        handle_ws_message(conn, is_binary, payload);
      },
      [this](std::uint64_t id) {
        conn_owner_.erase(id);
        for (auto& [symbol, subs] : md_subscribers_) {
          subs.erase(id);
        }
      },
      [this](WsConnection& conn, const HttpRequest& handshake) {
        on_ws_open(conn, handshake);
      });
  const auto ws_port_cfg = static_cast<std::uint16_t>(
      config_.get_int("net.ws_port").value());
  if (Status s = ws_->listen(addr, ws_port_cfg); !s.ok()) {
    return s;
  }

  const std::int64_t interval_ms =
      config_.get_int("storage.commit_interval_ms").value();
  commit_timer_ = loop_.add_timer(interval_ms * 1'000'000, /*repeat=*/true,
                                  this);

  std::ostringstream msg;
  msg << "codicis listening: http " << addr << ":" << http_->port()
      << ", ws " << addr << ":" << ws_->port();
  LogMessage(LogLevel::kInfo, msg.str());
  return Status::Ok();
}

std::uint16_t AppServer::http_port() const {
  return http_ ? http_->port() : 0;
}

std::uint16_t AppServer::ws_port() const { return ws_ ? ws_->port() : 0; }

void AppServer::on_timer(TimerId /*id*/) {
  if (storage_ && storage_->processed_pending() > 0) {
    engine_->commit();
  }
}

void AppServer::setup_routes() {
  router_.add("GET", "/health", [](const HttpRequest&, HttpResponse& resp) {
    resp.set_status(200);
    resp.set_header("Content-Type", "text/plain");
    resp.body = "ok";
  });
  router_.add_async("POST", "/orders",
                    [this](const HttpRequest& r, const HttpResponder& respond) {
                      handle_submit(r, respond);
                    });
  router_.add_async("POST", "/orders/cancel",
                    [this](const HttpRequest& r, const HttpResponder& respond) {
                      handle_cancel(r, respond);
                    });
  router_.add("GET", "/book", [this](const HttpRequest& r, HttpResponse& p) {
    handle_book(r, p);
  });
}

void AppServer::handle_submit(const HttpRequest& req,
                              const HttpResponder& respond) {
  // Parse the request synchronously (the request is not valid after we return).
  Symbol symbol;
  Order order;  // the engine assigns the id
  std::string err;
  if (!ParseOrderForm(ParseForm(req.body), &symbol, &order, &err)) {
    HttpResponse resp;
    JsonError(resp, 400, err);
    respond(std::move(resp));
    return;
  }

  // The owner comes only from the authenticated identity, never the body.
  authenticate(req, [this, symbol, order, respond](
                        bool ok, int status, const std::string& owner,
                        const std::string& auth_err) {
    if (!ok) {
      HttpResponse resp;
      JsonError(resp, status, auth_err);
      respond(std::move(resp));
      return;
    }
    // Report-before-place: the engine reports the order to storage first, and
    // reports the resulting trades and fills; we respond once it has placed.
    engine_->submit(
        symbol, owner, order,
        [this, symbol, respond](const TradingEngine::Result& res) {
          HttpResponse resp;
          resp.set_header("Content-Type", "application/json");
          if (!res.storage_ok) {
            resp.set_status(503);
          } else if (!res.outcome.accepted) {
            resp.set_status(409);
          } else {
            resp.set_status(200);
          }
          resp.body = ResultBody(res);
          respond(std::move(resp));
          if (res.storage_ok && res.outcome.accepted) {
            broadcast_market_data(symbol, res.outcome.trades);
          }
        });
  });
}

void AppServer::handle_ws_message(WsConnection& conn, bool /*is_binary*/,
                                  std::string_view payload) {
  const auto form = ParseForm(payload);

  // A per-symbol market-data subscription request (rather than an order).
  if (const std::string* action = FormGet(form, "action")) {
    const std::string* sym = FormGet(form, "symbol");
    if (sym == nullptr || sym->empty()) {
      conn.send_text(JsonErrorBody("missing symbol"));
      return;
    }
    if (*action == "subscribe") {
      md_subscribers_[*sym][conn.id()] = conn.fd();
      conn.send_text("{\"subscribed\":true}");
      return;
    }
    if (*action == "unsubscribe") {
      md_subscribers_[*sym].erase(conn.id());
      conn.send_text("{\"subscribed\":false}");
      return;
    }
  }

  // Otherwise submit an order (same form-encoded body as REST). The engine
  // callback fires later; route the reply back through the server keyed by
  // (fd, id) so it is dropped safely if the connection is gone.
  Symbol symbol;
  Order order;
  std::string err;
  if (!ParseOrderForm(form, &symbol, &order, &err)) {
    conn.send_text(JsonErrorBody(err));
    return;
  }
  // Identity is per-connection, resolved once at the handshake (Option A/B) or
  // anonymous when auth is disabled. A missing entry means it did not resolve.
  const auto oit = conn_owner_.find(conn.id());
  if (oit == conn_owner_.end()) {
    conn.send_text(JsonErrorBody("unauthenticated"));
    return;
  }
  const std::string owner = oit->second;
  const int fd = conn.fd();
  const std::uint64_t id = conn.id();
  engine_->submit(
      symbol, owner, order,
      [this, symbol, fd, id](const TradingEngine::Result& res) {
        ws_->deliver_text(fd, id, ResultBody(res));
        if (res.storage_ok && res.outcome.accepted) {
          broadcast_market_data(symbol, res.outcome.trades);
        }
      });
}

void AppServer::handle_cancel(const HttpRequest& req,
                              const HttpResponder& respond) {
  const auto form = ParseForm(req.body);
  const std::string* order_uuid = FormGet(form, "order");
  if (order_uuid == nullptr || !IsValidUuidString(*order_uuid)) {
    HttpResponse resp;
    JsonError(resp, 400, "missing or invalid order");
    respond(std::move(resp));
    return;
  }
  const std::string handle = *order_uuid;

  // The requester's identity comes from authentication; authorization then
  // checks it against the resting order's owner.
  authenticate(req, [this, handle, respond](bool ok, int status,
                                            const std::string& owner,
                                            const std::string& auth_err) {
    HttpResponse resp;
    resp.set_header("Content-Type", "application/json");
    if (!ok) {
      JsonError(resp, status, auth_err);
      respond(std::move(resp));
      return;
    }
    Symbol symbol;
    switch (engine_->cancel(handle, owner, &symbol)) {
      case CancelResult::kOk:
        resp.set_status(200);
        resp.body = "{\"cancelled\":true}";
        broadcast_market_data(symbol, {});  // the top of book may have changed
        break;
      case CancelResult::kNotFound:
        JsonError(resp, 404, "unknown order");
        break;
      case CancelResult::kForbidden:
        JsonError(resp, 403, "not the order owner");
        break;
    }
    respond(std::move(resp));
  });
}

void AppServer::on_ws_open(WsConnection& conn, const HttpRequest& handshake) {
  const std::uint64_t id = conn.id();
  authenticate(handshake, [this, id](bool ok, int /*status*/,
                                     const std::string& owner,
                                     const std::string& /*err*/) {
    if (ok) {
      conn_owner_[id] = owner;  // "" when auth is disabled (anonymous)
    }
    // On failure the connection stays unauthenticated: order frames are
    // rejected until a valid identity is presented on a new connection.
    // Market-data subscriptions remain available regardless.
  });
}

void AppServer::authenticate(const HttpRequest& req, AuthFn cb) {
  if (!auth_header_enabled_ && !auth_helper_enabled_) {
    cb(true, 200, std::string(), std::string());  // anonymous
    return;
  }

  // Option A: a user UUID in a header trusted from an authenticating edge.
  std::string header_uuid;
  if (auth_header_enabled_) {
    const std::string* h = req.header(auth_header_name_);
    if (h == nullptr || !IsValidUuidString(*h)) {
      cb(false, 401, std::string(), "missing or invalid identity header");
      return;
    }
    header_uuid = *h;
  }

  if (!auth_helper_enabled_) {
    cb(true, 200, header_uuid, std::string());
    return;
  }

  // Option B: validate a credential header via the auth helper pool.
  const std::string* cred = req.header(auth_credential_header_);
  if (cred == nullptr || cred->empty()) {
    cb(false, 401, std::string(), "missing credential");
    return;
  }
  const bool have_header = auth_header_enabled_;
  auth_->resolve(*cred, [cb, header_uuid, have_header](
                            bool resolved, const std::string& helper_uuid) {
    if (!resolved) {
      cb(false, 403, std::string(), "authentication failed");
      return;
    }
    // Defense in depth: when both mechanisms are enabled they must agree.
    if (have_header && header_uuid != helper_uuid) {
      cb(false, 403, std::string(), "identity mismatch");
      return;
    }
    cb(true, 200, helper_uuid, std::string());
  });
}

void AppServer::handle_book(const HttpRequest& req, HttpResponse& resp) {
  const auto q = ParseForm(req.query);  // e.g. /book?symbol=BTC
  const std::string* sym = FormGet(q, "symbol");
  if (sym == nullptr || sym->empty()) {
    return JsonError(resp, 400, "missing symbol");
  }
  resp.set_status(200);
  resp.set_header("Content-Type", "application/json");
  resp.body = "{" + BookTopFields(matching_, *sym) + "}";
}

void AppServer::broadcast_market_data(const Symbol& symbol,
                                      const std::vector<Trade>& trades) {
  const auto it = md_subscribers_.find(symbol);
  if (it == md_subscribers_.end() || it->second.empty()) {
    return;
  }
  std::ostringstream m;
  m << "{\"type\":\"md\"," << BookTopFields(matching_, symbol) << ",\"trades\":[";
  for (std::size_t i = 0; i < trades.size(); ++i) {
    if (i != 0) {
      m << ",";
    }
    m << "{\"price\":" << trades[i].price << ",\"qty\":" << trades[i].qty
      << "}";
  }
  m << "]}";
  const std::string msg = m.str();
  for (const auto& [id, fd] : it->second) {
    ws_->deliver_text(fd, id, msg);
  }
}

}  // namespace codicis
