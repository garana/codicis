/**
 * @file options.cc
 * @brief Implementation of BuildOptionRegistry (see options.h).
 */

#include "codicis/app/options.h"

namespace codicis {

OptionRegistry BuildOptionRegistry() {
  OptionRegistry reg;
  reg.add_int_range("net.http_port", 8080, 0, 65535,
                    "TCP port for the HTTP REST API (0 = ephemeral)");
  reg.add_int_range("net.ws_port", 8081, 0, 65535,
                    "TCP port for the WebSocket API (0 = ephemeral)");
  reg.add_string("net.bind_address", "127.0.0.1",
                 "IPv4 address to bind listeners to");
  reg.add_int_range("book.mem_levels", 100, 1, 1'000'000,
                    "Max in-memory price levels per side (top of book)");
  reg.add_string("storage.helper_cmd", "./codicis_storage_helper",
                 "Command used to launch the storage helper child process");
  reg.add_string("storage.codec", "text",
                 "Helper wire codec: 'text' or 'binary'");
  reg.add_int_range("storage.commit_interval_ms", 100, 1, 3'600'000,
                    "How often to ask the storage helper to commit");
  reg.add_int_range("storage.request_timeout_ms", 5000, 0, 3'600'000,
                    "Per-request storage helper timeout in ms (0 disables)");
  reg.add_string("log.level", "info",
                 "Minimum log level: trace|debug|info|warn|error");

  // Authentication (identity of the requesting user). Two independent,
  // non-exclusive mechanisms; when both are on, both must pass and agree.
  // Option A: trust a user UUID in a header set by an authenticating edge.
  reg.add_bool("auth.header.enabled", false,
               "Trust a pre-authenticated user UUID from a request header");
  reg.add_string("auth.header.name", "X-User-Id",
                 "Request header carrying the pre-authenticated user UUID");
  // Option B: validate a credential header via a helper process pool.
  reg.add_bool("auth.helper.enabled", false,
               "Validate a credential header via an auth helper process");
  reg.add_string("auth.helper.cmd", "./codicis_auth_helper",
                 "Command used to launch the auth helper child process(es)");
  reg.add_int_range("auth.helper.concurrency", 1, 1, 1024,
                    "Number of concurrent auth helper processes (pool size)");
  reg.add_bool("auth.helper.pipelining", true,
               "Whether the auth helper accepts overlapping in-flight requests");
  reg.add_int_range("auth.helper.pipeline_depth", 8, 1, 65536,
                    "Max in-flight requests per helper (forced to 1 if no "
                    "pipelining)");
  reg.add_string("auth.helper.credential_header", "Authorization",
                 "Request header whose value is forwarded to the auth helper");
  reg.add_int_range("auth.helper.request_timeout_ms", 5000, 0, 3'600'000,
                    "Per-request auth helper timeout in ms (0 disables)");
  reg.add_int_range("auth.cache.max_entries", 4096, 0, 100'000'000,
                    "Positive auth cache capacity (validated credentials)");
  reg.add_int_range("auth.cache.ttl_ms", 60'000, 0, 3'600'000,
                    "Positive auth cache entry lifetime in ms");
  reg.add_int_range("auth.cache.negative_max_entries", 1024, 0, 100'000'000,
                    "Negative auth cache capacity (rejected credentials)");
  reg.add_int_range("auth.cache.negative_ttl_ms", 5000, 0, 3'600'000,
                    "Negative auth cache entry lifetime in ms (keep short)");
  return reg;
}

}  // namespace codicis
