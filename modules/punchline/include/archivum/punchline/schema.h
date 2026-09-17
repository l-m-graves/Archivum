// The Punchline schema as migrations (docs/punchline-schema.md).
#pragma once

#include <vector>

#include "archivum/engine/migrate.h"

namespace archivum::punchline {

// Every migration of the Punchline module, in order. Applied with
// archivum::engine::migrate; `archivum migrate --db <path>` does that.
const std::vector<engine::Migration>& migrations();

// Names of the tables migration 1 creates, in creation order.
const std::vector<std::string>& v1_tables();

}  // namespace archivum::punchline
