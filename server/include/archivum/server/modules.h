// The server-side module framework: a module's data layer (core::Module:
// migrations and record policy) plus its routes. The App applies every
// module's migrations at startup, builds the record policy once, and
// registers the routes.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "archivum/core/module.h"
#include "archivum/status.h"

namespace archivum::server {

class App;

struct ServerModule {
  const core::Module* data = nullptr;
  // Parses `modules.<name>` from the configuration with the module's own
  // allow-list; called at configure() with an empty object when the
  // section is absent, so every module has defaults and refuses unknown
  // keys. Fails closed like the core sections.
  std::function<Status(const nlohmann::json&)> configure;
  std::function<void(App&)> register_routes;
  // Started after authentication is initialised, stopped before the
  // store closes: background work such as freshness monitoring.
  std::function<void(App&)> start;
  std::function<void()> stop;
  // One pass of the module's background work, for tests and operators;
  // returns how many items it produced.
  std::function<Result<int>(App&)> run_once;
};

// The modules this binary ships: Punchline.
std::vector<ServerModule> builtin_modules();

}  // namespace archivum::server
