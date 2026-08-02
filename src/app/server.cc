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
    const std::vector<std::pair<std::string, std::string>>& form, Order* out,
    std::string* err) {
  const std::string* side_s = FormGet(form, "side");
  const std::string* type_s = FormGet(form, "type");
  const std::string* qty_s = FormGet(form, "qty");
  Side side;
  OrdType type;
  std::int64_t qty = 0;
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
  *out = order;
  return true;
}

/** @brief JSON body describing a successful submit outcome. */
std::string SubmitBody(const SubmitOutcome& out) {
  std::ostringstream body;
  body << "{\"accepted\":true,\"order_id\":" << out.order_id
       << ",\"filled\":" << out.filled
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
  return SubmitBody(res.outcome);
}

/** @brief Top-of-book JSON fields: "bid",... ,"ask",... (no braces). */
std::string BookTopFields(const OrderBook& book) {
  std::ostringstream f;
  Ticks bid = 0;
  Ticks ask = 0;
  if (book.best_bid(&bid)) {
    f << "\"bid\":" << bid
      << ",\"bid_qty\":" << book.total_qty_at(Side::Buy, bid);
  } else {
    f << "\"bid\":null,\"bid_qty\":0";
  }
  f << ",";
  if (book.best_ask(&ask)) {
    f << "\"ask\":" << ask
      << ",\"ask_qty\":" << book.total_qty_at(Side::Sell, ask);
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
  engine_ = std::make_unique<TradingEngine>(book_, *storage_);

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
      [this](std::uint64_t id) { md_subscribers_.erase(id); });
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
                    [this](const HttpRequest& r, HttpResponder respond) {
                      handle_submit(r, std::move(respond));
                    });
  router_.add("POST", "/orders/cancel",
              [this](const HttpRequest& r, HttpResponse& p) {
                handle_cancel(r, p);
              });
  router_.add("GET", "/book", [this](const HttpRequest& r, HttpResponse& p) {
    handle_book(r, p);
  });
}

void AppServer::handle_submit(const HttpRequest& req, HttpResponder respond) {
  // Parse the request synchronously (the request is not valid after we return).
  Order order;  // the engine assigns the id
  std::string err;
  if (!ParseOrderForm(ParseForm(req.body), &order, &err)) {
    HttpResponse resp;
    JsonError(resp, 400, err);
    respond(std::move(resp));
    return;
  }

  // Report-before-place: the engine reports the order to storage first, and
  // reports the resulting trades and fills; we respond once it has placed.
  engine_->submit(order, [this, respond](const TradingEngine::Result& res) {
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
      broadcast_market_data(res.outcome.trades);
    }
  });
}

void AppServer::handle_ws_message(WsConnection& conn, bool /*is_binary*/,
                                  std::string_view payload) {
  const auto form = ParseForm(payload);

  // A market-data subscription request (rather than an order).
  if (const std::string* action = FormGet(form, "action")) {
    if (*action == "subscribe") {
      md_subscribers_[conn.id()] = conn.fd();
      conn.send_text("{\"subscribed\":true}");
      return;
    }
    if (*action == "unsubscribe") {
      md_subscribers_.erase(conn.id());
      conn.send_text("{\"subscribed\":false}");
      return;
    }
  }

  // Otherwise submit an order (same form-encoded body as REST). The engine
  // callback fires later; route the reply back through the server keyed by
  // (fd, id) so it is dropped safely if the connection is gone.
  Order order;
  std::string err;
  if (!ParseOrderForm(form, &order, &err)) {
    conn.send_text(JsonErrorBody(err));
    return;
  }
  const int fd = conn.fd();
  const std::uint64_t id = conn.id();
  engine_->submit(order, [this, fd, id](const TradingEngine::Result& res) {
    ws_->deliver_text(fd, id, ResultBody(res));
    if (res.storage_ok && res.outcome.accepted) {
      broadcast_market_data(res.outcome.trades);
    }
  });
}

void AppServer::handle_cancel(const HttpRequest& req, HttpResponse& resp) {
  const auto form = ParseForm(req.body);
  const std::string* id_s = FormGet(form, "id");
  std::int64_t id = 0;
  if (id_s == nullptr || !ParseI64(*id_s, &id) || id <= 0) {
    return JsonError(resp, 400, "missing or invalid id");
  }
  const bool ok = book_.cancel(static_cast<OrderId>(id));
  resp.set_status(ok ? 200 : 404);
  resp.set_header("Content-Type", "application/json");
  resp.body = std::string("{\"cancelled\":") + (ok ? "true" : "false") + "}";
  if (ok) {
    broadcast_market_data({});  // the top of book may have changed
  }
}

void AppServer::handle_book(const HttpRequest& /*req*/, HttpResponse& resp) {
  resp.set_status(200);
  resp.set_header("Content-Type", "application/json");
  resp.body = "{" + BookTopFields(book_) + "}";
}

void AppServer::broadcast_market_data(const std::vector<Trade>& trades) {
  if (md_subscribers_.empty()) {
    return;
  }
  std::ostringstream m;
  m << "{\"type\":\"md\"," << BookTopFields(book_) << ",\"trades\":[";
  for (std::size_t i = 0; i < trades.size(); ++i) {
    if (i != 0) {
      m << ",";
    }
    m << "{\"price\":" << trades[i].price << ",\"qty\":" << trades[i].qty
      << "}";
  }
  m << "]}";
  const std::string msg = m.str();
  for (const auto& [id, fd] : md_subscribers_) {
    ws_->deliver_text(fd, id, msg);
  }
}

}  // namespace codicis
