#include "archivum/core/authz.h"

#include "archivum/core/ids.h"
#include "archivum/core/schema.h"

namespace archivum::core {
namespace {
using engine::Bound;
using engine::Row;
using engine::Value;
}  // namespace

Result<std::set<std::string>> roles_for(engine::Reader& reader, const std::string& tid, const std::string& oid) {
  std::set<std::string> out;
  if (tid.empty() || oid.empty()) return out;
  auto rows = reader.scan_all(kRoleGrants, "role_grants_principal", Bound{{Value::text(tid), Value::text(oid)}},
                              Bound{{Value::text(tid), Value::text(oid)}});
  if (!rows.ok()) return rows.status();
  for (const Row& r : rows.value()) out.insert(r[roles::kRole].as_text());
  return out;
}

Result<std::set<std::string>> datasets_for(engine::Reader& reader, const std::string& tid, const std::string& oid,
                                           const std::string& permission) {
  std::set<std::string> out;
  if (tid.empty() || oid.empty()) return out;
  auto rows = reader.scan_all(kDatasetGrants, "dataset_grants_principal", Bound{{Value::text(tid), Value::text(oid)}},
                              Bound{{Value::text(tid), Value::text(oid)}});
  if (!rows.ok()) return rows.status();
  for (const Row& r : rows.value()) {
    if (r[datasets::kPermission].as_text() == permission) out.insert(r[datasets::kDataset].as_text());
  }
  return out;
}

Result<std::int64_t> grant_role(Recorder& rec, const std::string& tid, const std::string& oid, const std::string& role,
                                const std::string& granted_by, std::int64_t now_us) {
  auto id = next_id(rec.writer(), kRoleGrants);
  if (!id.ok()) return id.status();
  Row r(roles::kColumns);
  r[roles::kId] = Value::integer(id.value());
  r[roles::kTid] = Value::text(tid);
  r[roles::kOid] = Value::text(oid);
  r[roles::kRole] = Value::text(role);
  r[roles::kGrantedBy] = Value::text(granted_by);
  r[roles::kGrantedAt] = Value::timestamp(now_us);
  if (Status s = rec.insert(kRoleGrants, r); !s.ok()) return s;
  return id.value();
}

Status revoke_role(Recorder& rec, std::int64_t grant_id) { return rec.remove(kRoleGrants, {Value::integer(grant_id)}); }

Result<std::int64_t> grant_dataset(Recorder& rec, const std::string& tid, const std::string& oid, const std::string& dataset,
                                   const std::string& permission, const std::string& granted_by, std::int64_t now_us) {
  auto id = next_id(rec.writer(), kDatasetGrants);
  if (!id.ok()) return id.status();
  Row r(datasets::kColumns);
  r[datasets::kId] = Value::integer(id.value());
  r[datasets::kTid] = Value::text(tid);
  r[datasets::kOid] = Value::text(oid);
  r[datasets::kDataset] = Value::text(dataset);
  r[datasets::kPermission] = Value::text(permission);
  r[datasets::kGrantedBy] = Value::text(granted_by);
  r[datasets::kGrantedAt] = Value::timestamp(now_us);
  if (Status s = rec.insert(kDatasetGrants, r); !s.ok()) return s;
  return id.value();
}

Status revoke_dataset(Recorder& rec, std::int64_t grant_id) {
  return rec.remove(kDatasetGrants, {Value::integer(grant_id)});
}

}  // namespace archivum::core
