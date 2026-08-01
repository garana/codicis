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
  reg.add_string("log.level", "info",
                 "Minimum log level: trace|debug|info|warn|error");
  return reg;
}

}  // namespace codicis
