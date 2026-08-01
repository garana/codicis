/**
 * @file main.cc
 * @brief codicis entry point: load config, wire the server, run the loop.
 */

#include <cstdio>
#include <memory>

#include "codicis/app/options.h"
#include "codicis/app/server.h"
#include "codicis/config/config.h"
#include "codicis/event/event_loop.h"
#include "codicis/util/clock.h"
#include "codicis/util/logging.h"

int main(int argc, char** argv) {
  using namespace codicis;

  const OptionRegistry registry = BuildOptionRegistry();
  Result<Config> cfg = Config::load(registry, argc, argv);
  if (!cfg.ok()) {
    std::fprintf(stderr, "configuration error: %s\n",
                 cfg.error().message.c_str());
    return 2;
  }
  const Config& config = cfg.value();

  LogLevel level = LogLevel::kInfo;
  if (ParseLogLevel(config.get_string("log.level").value(), &level)) {
    SetLogLevel(level);
  }

  SystemClock clock;
  Result<std::unique_ptr<EventLoop>> loop = MakeEventLoop(&clock);
  if (!loop.ok()) {
    std::fprintf(stderr, "event loop error: %s\n",
                 loop.error().message.c_str());
    return 1;
  }

  AppServer server(*loop.value(), config);
  if (Status s = server.start(); !s.ok()) {
    std::fprintf(stderr, "startup error: %s\n", s.error().message.c_str());
    return 1;
  }

  loop.value()->run();
  return 0;
}
