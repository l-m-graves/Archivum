// Schema migrations: per module, an ordered list of versioned steps
// applied to a Store, each in its own write transaction, recorded in the
// table `archivum_migrations` (keyed by module and version) and counted in
// the catalog's schema version, which is the total number of steps applied
// across modules.
//
// Rules:
//   * within a module versions are 1, 2, 3, ... with no gaps; names are
//     stable;
//   * a store with N rows for a module has rows 1..N whose names match the
//     module's list, else the list and the store have diverged and nothing
//     is applied;
//   * a store ahead of the list (N > last version) is refused: the binary
//     is older than the database;
//   * each pending step runs as one Writer: the step's changes, its row
//     in `archivum_migrations`, and the schema version commit together or
//     not at all. A crash between steps leaves the store at a step
//     boundary and the next run continues from there.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "archivum/engine/store.h"

namespace archivum::engine {

struct Migration {
  std::uint64_t version = 0;
  std::string name;
  std::function<Status(Writer&)> apply;
};

struct MigrationReport {
  std::uint64_t from_version = 0;  // the module's applied count before
  std::uint64_t to_version = 0;    // and after
  std::vector<std::string> applied;  // names, in order
};

constexpr const char* kMigrationsTable = "archivum_migrations";

Result<MigrationReport> migrate(Store& store, const std::string& module, const std::vector<Migration>& migrations);

// The applied steps of `module`: (version, name), in order.
Result<std::vector<std::pair<std::uint64_t, std::string>>> applied_migrations(Reader& reader, const std::string& module);

// Checks the list's shape (versions consecutive from 1, names non-empty
// and distinct) without touching a store.
Status validate_migrations(const std::vector<Migration>& migrations);

}  // namespace archivum::engine
