// The module interface: a module owns a schema (its migrations under its
// own name) and declares which of its columns the recorder may record by
// value. Routes are the server's concern (server/include/.../modules.h),
// so a module's data layer is testable without Drogon.
#pragma once

#include <string>
#include <vector>

#include "archivum/core/recorder.h"
#include "archivum/engine/migrate.h"

namespace archivum::core {

class Module {
 public:
  virtual ~Module() = default;
  virtual std::string name() const = 0;
  virtual const std::vector<engine::Migration>& migrations() const = 0;
  // Adds the module's recordable columns. A column not added is recorded
  // as presence and shape only.
  virtual void extend_policy(RecordPolicy& policy) const = 0;
};

struct SchemaReport {
  std::vector<std::pair<std::string, engine::MigrationReport>> modules;  // core first
};

// Applies the core migrations and then every module's, in order.
Result<SchemaReport> migrate_all(engine::Store& store, const std::vector<const Module*>& modules);

// The core's own recordable columns plus every module's.
RecordPolicy build_policy(const std::vector<const Module*>& modules);

}  // namespace archivum::core
