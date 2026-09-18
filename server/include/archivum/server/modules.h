// The server-side module framework: a module's data layer (core::Module:
// migrations and record policy) plus its routes. The App applies every
// module's migrations at startup, builds the record policy once, and
// registers the routes.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "archivum/core/module.h"

namespace archivum::server {

class App;

struct ServerModule {
  const core::Module* data = nullptr;
  std::function<void(App&)> register_routes;
};

// The modules this binary ships: Punchline.
std::vector<ServerModule> builtin_modules();

}  // namespace archivum::server
