#include "archivum/core/module.h"

#include "archivum/core/schema.h"

namespace archivum::core {

Result<SchemaReport> migrate_all(engine::Store& store, const std::vector<const Module*>& modules) {
  SchemaReport rep;
  auto core = engine::migrate(store, kModule, migrations());
  if (!core.ok()) return core.status();
  rep.modules.emplace_back(kModule, core.value());
  for (const Module* m : modules) {
    auto r = engine::migrate(store, m->name(), m->migrations());
    if (!r.ok()) return r.status();
    rep.modules.emplace_back(m->name(), r.value());
  }
  return rep;
}

RecordPolicy build_policy(const std::vector<const Module*>& modules) {
  RecordPolicy p;
  // Core tables hold no Synthex data; their identifying and state columns
  // are recordable. Verifiers are blobs and are never recorded by value.
  p.allow(kRoleGrants, {"id", "tid", "oid", "role", "granted_by", "granted_at"});
  p.allow(kDatasetGrants, {"id", "tid", "oid", "dataset", "permission", "granted_by", "granted_at"});
  p.allow(kLocalAccounts, {"id", "username", "kind", "enabled", "created_at", "expires_at", "last_used_at"});
  for (const Module* m : modules) m->extend_policy(p);
  return p;
}

}  // namespace archivum::core
