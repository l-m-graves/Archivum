#include "archivum/core/schema.h"

namespace archivum::core {
namespace {

using engine::CheckDef;
using engine::CheckOp;
using engine::ColumnDef;
using engine::ColumnType;
using engine::ForeignKeyDef;
using engine::IndexDef;
using engine::TableDef;
using engine::Value;
using engine::Writer;

ColumnDef col(const char* name, ColumnType type, bool nullable = false) { return ColumnDef{name, type, nullable, 0}; }
IndexDef index(const char* name, std::vector<std::string> columns, bool unique = false) {
  return IndexDef{name, std::move(columns), unique, 0};
}
CheckDef one_of(const char* name, const char* column, std::vector<const char*> values) {
  CheckDef c;
  c.name = name;
  c.column = column;
  c.op = CheckOp::In;
  for (const char* v : values) c.operands.push_back(Value::text(v));
  return c;
}
CheckDef non_empty(const char* name, const char* column) {
  return CheckDef{name, column, CheckOp::Ne, {Value::text("")}, ""};
}

TableDef audit_log() {
  TableDef t;
  t.name = kAuditLog;
  t.columns = {col("id", ColumnType::Integer),
               col("at", ColumnType::Timestamp),
               col("actor_kind", ColumnType::Text),
               col("actor_tid", ColumnType::Text, true),
               col("actor_oid", ColumnType::Text, true),
               col("actor_account", ColumnType::Text, true),
               col("actor_device", ColumnType::Uuid, true),
               col("action", ColumnType::Text),
               col("target_table", ColumnType::Text, true),
               col("target_id", ColumnType::Text, true),
               col("request_id", ColumnType::Text, true),
               col("detail", ColumnType::Text, true)};
  t.primary_key = {"id"};
  t.indexes = {index("audit_log_at", {"at"}), index("audit_log_target", {"target_table", "target_id"}),
               index("audit_log_actor", {"actor_tid", "actor_oid"}), index("audit_log_device", {"actor_device"}),
               index("audit_log_account", {"actor_account"})};
  t.checks = {one_of("audit_log_actor_kind_known", "actor_kind", {"principal", "device", "account", "system"}),
              non_empty("audit_log_action_nonempty", "action")};
  return t;
}

TableDef change_feed() {
  TableDef t;
  t.name = kChangeFeed;
  t.columns = {col("id", ColumnType::Integer),
               col("audit_id", ColumnType::Integer),
               col("at", ColumnType::Timestamp),
               col("table_name", ColumnType::Text),
               col("op", ColumnType::Text),
               col("key", ColumnType::Text),
               col("before_image", ColumnType::Text, true),
               col("after_image", ColumnType::Text, true)};
  t.primary_key = {"id"};
  t.indexes = {index("change_feed_audit", {"audit_id"}), index("change_feed_table", {"table_name", "id"})};
  t.foreign_keys = {ForeignKeyDef{"change_feed_audit_fk", {"audit_id"}, kAuditLog, {"id"}}};
  t.checks = {one_of("change_feed_op_known", "op", {"insert", "update", "delete"}),
              non_empty("change_feed_table_nonempty", "table_name")};
  return t;
}

TableDef role_grants() {
  TableDef t;
  t.name = kRoleGrants;
  t.columns = {col("id", ColumnType::Integer),  col("tid", ColumnType::Text),
               col("oid", ColumnType::Text),    col("role", ColumnType::Text),
               col("granted_by", ColumnType::Text), col("granted_at", ColumnType::Timestamp)};
  t.primary_key = {"id"};
  t.indexes = {index("role_grants_principal", {"tid", "oid", "role"}, true), index("role_grants_role", {"role"})};
  t.checks = {one_of("role_grants_role_known", "role", {"supervisor", "payroll", "admin", "analyst"}),
              non_empty("role_grants_tid_nonempty", "tid"), non_empty("role_grants_oid_nonempty", "oid")};
  return t;
}

TableDef dataset_grants() {
  TableDef t;
  t.name = kDatasetGrants;
  t.columns = {col("id", ColumnType::Integer),      col("tid", ColumnType::Text),
               col("oid", ColumnType::Text),        col("dataset", ColumnType::Text),
               col("permission", ColumnType::Text), col("granted_by", ColumnType::Text),
               col("granted_at", ColumnType::Timestamp)};
  t.primary_key = {"id"};
  t.indexes = {index("dataset_grants_principal", {"tid", "oid", "dataset", "permission"}, true),
               index("dataset_grants_dataset", {"dataset"})};
  t.checks = {one_of("dataset_grants_permission_known", "permission", {"read", "write", "admin"}),
              non_empty("dataset_grants_dataset_nonempty", "dataset"), non_empty("dataset_grants_tid_nonempty", "tid"),
              non_empty("dataset_grants_oid_nonempty", "oid")};
  return t;
}

TableDef local_accounts() {
  TableDef t;
  t.name = kLocalAccounts;
  t.columns = {col("id", ColumnType::Integer),
               col("username", ColumnType::Text),
               col("kind", ColumnType::Text),
               col("verifier", ColumnType::Blob),
               col("enabled", ColumnType::Boolean),
               col("created_at", ColumnType::Timestamp),
               col("expires_at", ColumnType::Timestamp, true),
               col("last_used_at", ColumnType::Timestamp, true)};
  t.primary_key = {"id"};
  t.indexes = {index("local_accounts_username", {"username"}, true)};
  t.checks = {one_of("local_accounts_kind_known", "kind", {"break_glass", "device_admin"}),
              non_empty("local_accounts_username_nonempty", "username")};
  return t;
}

Status apply_v1(Writer& w) {
  for (const TableDef& t : {audit_log(), change_feed(), role_grants(), dataset_grants(), local_accounts()}) {
    if (Status s = w.create_table(t); !s.ok()) return s;
  }
  return Status();
}

}  // namespace

const std::vector<engine::Migration>& migrations() {
  static const std::vector<engine::Migration> list = {engine::Migration{1, "core_v1", &apply_v1}};
  return list;
}

}  // namespace archivum::core
