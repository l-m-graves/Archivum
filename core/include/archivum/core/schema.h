// The core schema: tables every module relies on, owned by the module
// named "archivum" (docs/audit-and-change-feed.md, docs/server-core.md).
//
//   audit_log       one row per recorded action: who, what, when, request
//   change_feed     one row per row change inside that action: table,
//                   key, operation, before and after images under the
//                   recording policy; references its audit row
//   role_grants     elevated roles keyed on (tid, oid)
//   dataset_grants  dataset permissions keyed on (tid, oid), as data
//   local_accounts  break-glass and device-administration accounts
#pragma once

#include <string>
#include <vector>

#include "archivum/engine/migrate.h"

namespace archivum::core {

constexpr const char* kModule = "archivum";
const std::vector<engine::Migration>& migrations();

constexpr const char* kAuditLog = "audit_log";
constexpr const char* kChangeFeed = "change_feed";
constexpr const char* kRoleGrants = "role_grants";
constexpr const char* kDatasetGrants = "dataset_grants";
constexpr const char* kLocalAccounts = "local_accounts";

// Column order of the core tables, for building rows.
namespace audit {
enum : std::size_t { kId, kAt, kActorKind, kActorTid, kActorOid, kActorAccount, kActorDevice, kAction, kTargetTable, kTargetId, kRequestId, kDetail, kColumns };
}
namespace feed {
enum : std::size_t { kId, kAuditId, kAt, kTable, kOp, kKey, kBefore, kAfter, kColumns };
}
namespace roles {
enum : std::size_t { kId, kTid, kOid, kRole, kGrantedBy, kGrantedAt, kColumns };
}
namespace datasets {
enum : std::size_t { kId, kTid, kOid, kDataset, kPermission, kGrantedBy, kGrantedAt, kColumns };
}
namespace accounts {
enum : std::size_t { kId, kUsername, kKind, kVerifier, kEnabled, kCreatedAt, kExpiresAt, kLastUsedAt, kColumns };
}

}  // namespace archivum::core
