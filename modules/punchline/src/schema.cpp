#include "archivum/punchline/schema.h"

namespace archivum::punchline {
namespace {

using engine::CheckDef;
using engine::CheckOp;
using engine::ColumnDef;
using engine::ColumnType;
using engine::ForeignKeyDef;
using engine::IndexDef;
using engine::Migration;
using archivum::Status;
using engine::TableDef;
using engine::Value;
using engine::Writer;

ColumnDef col(const char* name, ColumnType type, bool nullable = false, std::uint8_t scale = 0) {
  return ColumnDef{name, type, nullable, scale};
}
IndexDef index(const char* name, std::vector<std::string> columns, bool unique = false) {
  return IndexDef{name, std::move(columns), unique, 0};
}
ForeignKeyDef fk(const char* name, std::vector<std::string> columns, const char* ref_table,
                 std::vector<std::string> ref_columns) {
  return ForeignKeyDef{name, std::move(columns), ref_table, std::move(ref_columns)};
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
  return CheckDef{name, column, CheckOp::Ne, {Value::text("")}};
}
CheckDef at_least(const char* name, const char* column, std::int64_t v) {
  return CheckDef{name, column, CheckOp::Ge, {Value::integer(v)}};
}
CheckDef at_most(const char* name, const char* column, std::int64_t v) {
  return CheckDef{name, column, CheckOp::Le, {Value::integer(v)}};
}

// Lifecycle states of an entry and of a period (punchline-updates.md):
// recorded -> submitted -> approved -> released -> locked.
const std::vector<const char*> kEntryStates = {"recorded", "submitted", "approved", "released", "locked"};
const std::vector<const char*> kPeriodStates = {"open", "submitted", "approved", "released", "locked"};

// Employees: the immutable employee id, the HR employee number, and the
// nullable Entra identity (tid, oid) beside it. Email never authorizes and
// carries no constraint beyond its type.
TableDef employees() {
  TableDef t;
  t.name = "employees";
  t.columns = {col("id", ColumnType::Integer),
               col("employee_number", ColumnType::Text),
               col("display_name", ColumnType::Text),
               col("email", ColumnType::Text, true),
               col("tid", ColumnType::Text, true),
               col("oid", ColumnType::Text, true),
               col("active", ColumnType::Boolean),
               col("pay_group", ColumnType::Text, true),
               col("site_zone", ColumnType::Text),
               col("created_at", ColumnType::Timestamp),
               col("updated_at", ColumnType::Timestamp)};
  t.primary_key = {"id"};
  t.indexes = {index("employees_number", {"employee_number"}, true),
               index("employees_identity", {"tid", "oid"}, true),
               index("employees_active_name", {"active", "display_name"})};
  t.checks = {non_empty("employees_number_nonempty", "employee_number"),
              non_empty("employees_name_nonempty", "display_name"),
              non_empty("employees_zone_nonempty", "site_zone")};
  return t;
}

// Elevated roles keyed on the Entra principal, never on email. Supervisor
// and payroll are separate grants; the segregation-of-duties report lists
// principals holding both.
TableDef role_grants() {
  TableDef t;
  t.name = "role_grants";
  t.columns = {col("id", ColumnType::Integer),
               col("tid", ColumnType::Text),
               col("oid", ColumnType::Text),
               col("role", ColumnType::Text),
               col("granted_by", ColumnType::Text),
               col("granted_at", ColumnType::Timestamp)};
  t.primary_key = {"id"};
  t.indexes = {index("role_grants_principal", {"tid", "oid", "role"}, true), index("role_grants_role", {"role"})};
  t.checks = {one_of("role_grants_role_known", "role", {"supervisor", "payroll", "admin"}),
              non_empty("role_grants_tid_nonempty", "tid"), non_empty("role_grants_oid_nonempty", "oid")};
  return t;
}

// Local accounts: the break-glass administrator (disabled by default,
// created by a host console command, expiring) and device administration.
// The verifier is an Argon2id hash; the schema stores bytes only.
TableDef local_accounts() {
  TableDef t;
  t.name = "local_accounts";
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

// One device, one credential, one employee. Revocation keeps the row (the
// audit trail references it) and sets revoked_at; enrollment logic keeps
// at most one unrevoked device per employee.
TableDef devices() {
  TableDef t;
  t.name = "devices";
  t.columns = {col("id", ColumnType::Integer),
               col("device_uuid", ColumnType::Uuid),
               col("name", ColumnType::Text),
               col("employee_id", ColumnType::Integer),
               col("credential_hash", ColumnType::Blob),
               col("enrolled_at", ColumnType::Timestamp),
               col("enrolled_by", ColumnType::Text),
               col("revoked_at", ColumnType::Timestamp, true),
               col("revoked_reason", ColumnType::Text, true),
               col("last_seen_at", ColumnType::Timestamp, true),
               col("last_acked_sequence", ColumnType::Integer, true)};
  t.primary_key = {"id"};
  t.indexes = {index("devices_uuid", {"device_uuid"}, true), index("devices_employee", {"employee_id", "revoked_at"})};
  t.foreign_keys = {fk("devices_employee_fk", {"employee_id"}, "employees", {"id"})};
  t.checks = {non_empty("devices_name_nonempty", "name")};
  return t;
}

// Pay periods in site-local days (days since 1970-01-01 in the site zone)
// with the tzdb version the boundaries were computed under.
TableDef pay_periods() {
  TableDef t;
  t.name = "pay_periods";
  t.columns = {col("id", ColumnType::Integer),
               col("start_day", ColumnType::Integer),
               col("end_day", ColumnType::Integer),
               col("site_zone", ColumnType::Text),
               col("tzdb_version", ColumnType::Text),
               col("state", ColumnType::Text),
               col("submit_by", ColumnType::Timestamp, true),
               col("approve_by", ColumnType::Timestamp, true),
               col("release_at", ColumnType::Timestamp, true)};
  t.primary_key = {"id"};
  t.indexes = {index("pay_periods_start", {"start_day"}, true), index("pay_periods_state", {"state"})};
  t.checks = {one_of("pay_periods_state_known", "state", kPeriodStates)};
  return t;
}

// Weekly schedules, HR-sourced, nullable in effect: an employee with no
// row is flagged "no schedule on file" rather than checked.
TableDef schedules() {
  TableDef t;
  t.name = "schedules";
  t.columns = {col("id", ColumnType::Integer),
               col("employee_id", ColumnType::Integer),
               col("weekday", ColumnType::Integer),
               col("start_minute", ColumnType::Integer),
               col("end_minute", ColumnType::Integer),
               col("effective_from_day", ColumnType::Integer),
               col("effective_to_day", ColumnType::Integer, true)};
  t.primary_key = {"id"};
  t.indexes = {index("schedules_employee", {"employee_id", "weekday", "effective_from_day"})};
  t.foreign_keys = {fk("schedules_employee_fk", {"employee_id"}, "employees", {"id"})};
  t.checks = {at_least("schedules_weekday_min", "weekday", 0), at_most("schedules_weekday_max", "weekday", 6),
              at_least("schedules_start_min", "start_minute", 0), at_most("schedules_start_max", "start_minute", 1439),
              at_least("schedules_end_min", "end_minute", 0), at_most("schedules_end_max", "end_minute", 1440)};
  return t;
}

// The audit log. The audit writer decides per field what is recordable
// (docs/confidentiality-check.md); the schema stores what it was given.
TableDef audit_log() {
  TableDef t;
  t.name = "audit_log";
  t.columns = {col("id", ColumnType::Integer),
               col("at", ColumnType::Timestamp),
               col("actor_tid", ColumnType::Text, true),
               col("actor_oid", ColumnType::Text, true),
               col("actor_account", ColumnType::Text, true),
               col("device_id", ColumnType::Integer, true),
               col("action", ColumnType::Text),
               col("target_table", ColumnType::Text),
               col("target_id", ColumnType::Text),
               col("fields", ColumnType::Text, true),
               col("request_id", ColumnType::Text, true)};
  t.primary_key = {"id"};
  t.indexes = {index("audit_log_at", {"at"}), index("audit_log_target", {"target_table", "target_id"}),
               index("audit_log_device", {"device_id"})};
  t.foreign_keys = {fk("audit_log_device_fk", {"device_id"}, "devices", {"id"})};
  t.checks = {non_empty("audit_log_action_nonempty", "action")};
  return t;
}

// Time entries: one row per punch. The entry fields from
// punchline-updates.md: attestation source, lifecycle state, device,
// employee, journal sequence, pay code, approval audit reference, receipt
// time, the three time values (device instant, local wall clock, receipt)
// and the tzdb version. The client-generated entry_uuid makes replay
// idempotent; (device, journal_sequence) is unique for the same reason.
TableDef time_entries() {
  TableDef t;
  t.name = "time_entries";
  t.columns = {col("id", ColumnType::Integer),
               col("entry_uuid", ColumnType::Uuid),
               col("employee_id", ColumnType::Integer),
               col("device_id", ColumnType::Integer),
               col("journal_sequence", ColumnType::Integer),
               col("kind", ColumnType::Text),
               col("device_time", ColumnType::Timestamp),
               col("local_time", ColumnType::Text),
               col("site_zone", ColumnType::Text),
               col("tzdb_version", ColumnType::Text),
               col("receipt_time", ColumnType::Timestamp),
               col("attestation", ColumnType::Text),
               col("clock_divergence_us", ColumnType::Integer, true),
               col("period_id", ColumnType::Integer, true),
               col("state", ColumnType::Text),
               col("pay_code", ColumnType::Text, true),
               col("approval_audit_id", ColumnType::Integer, true),
               col("correction_of", ColumnType::Integer, true),
               col("correction_reason", ColumnType::Text, true),
               col("created_at", ColumnType::Timestamp)};
  t.primary_key = {"id"};
  t.indexes = {index("time_entries_uuid", {"entry_uuid"}, true),
               index("time_entries_device_sequence", {"device_id", "journal_sequence"}, true),
               index("time_entries_employee_time", {"employee_id", "device_time"}),
               index("time_entries_period", {"period_id", "employee_id"}),
               index("time_entries_state", {"state"}),
               index("time_entries_correction", {"correction_of"}),
               index("time_entries_approval", {"approval_audit_id"})};
  t.foreign_keys = {fk("time_entries_employee_fk", {"employee_id"}, "employees", {"id"}),
                    fk("time_entries_device_fk", {"device_id"}, "devices", {"id"}),
                    fk("time_entries_period_fk", {"period_id"}, "pay_periods", {"id"}),
                    fk("time_entries_approval_fk", {"approval_audit_id"}, "audit_log", {"id"}),
                    fk("time_entries_correction_fk", {"correction_of"}, "time_entries", {"id"})};
  t.checks = {one_of("time_entries_kind_known", "kind", {"in", "out"}),
              one_of("time_entries_attestation_known", "attestation", {"device", "server", "manual"}),
              one_of("time_entries_state_known", "state", kEntryStates),
              at_least("time_entries_sequence_min", "journal_sequence", 0)};
  return t;
}

// Idempotent sync batches from devices: a replayed batch is answered from
// this table without re-inserting entries.
TableDef sync_batches() {
  TableDef t;
  t.name = "sync_batches";
  t.columns = {col("id", ColumnType::Integer),
               col("batch_uuid", ColumnType::Uuid),
               col("device_id", ColumnType::Integer),
               col("received_at", ColumnType::Timestamp),
               col("first_sequence", ColumnType::Integer),
               col("last_sequence", ColumnType::Integer),
               col("accepted", ColumnType::Integer),
               col("rejected", ColumnType::Integer)};
  t.primary_key = {"id"};
  t.indexes = {index("sync_batches_uuid", {"batch_uuid"}, true), index("sync_batches_device", {"device_id", "received_at"})};
  t.foreign_keys = {fk("sync_batches_device_fk", {"device_id"}, "devices", {"id"})};
  t.checks = {at_least("sync_batches_accepted_min", "accepted", 0), at_least("sync_batches_rejected_min", "rejected", 0)};
  return t;
}

// Approval history: one row per lifecycle transition of an employee's
// period, each referencing its audit entry.
TableDef approvals() {
  TableDef t;
  t.name = "approvals";
  t.columns = {col("id", ColumnType::Integer),
               col("period_id", ColumnType::Integer),
               col("employee_id", ColumnType::Integer),
               col("from_state", ColumnType::Text),
               col("to_state", ColumnType::Text),
               col("acted_by_tid", ColumnType::Text, true),
               col("acted_by_oid", ColumnType::Text, true),
               col("acted_by_account", ColumnType::Text, true),
               col("acted_at", ColumnType::Timestamp),
               col("note", ColumnType::Text, true),
               col("audit_id", ColumnType::Integer)};
  t.primary_key = {"id"};
  t.indexes = {index("approvals_period", {"period_id", "employee_id", "acted_at"}), index("approvals_audit", {"audit_id"}),
               index("approvals_employee", {"employee_id"})};
  t.foreign_keys = {fk("approvals_period_fk", {"period_id"}, "pay_periods", {"id"}),
                    fk("approvals_employee_fk", {"employee_id"}, "employees", {"id"}),
                    fk("approvals_audit_fk", {"audit_id"}, "audit_log", {"id"})};
  t.checks = {one_of("approvals_from_known", "from_state", kEntryStates), one_of("approvals_to_known", "to_state", kEntryStates)};
  return t;
}

// The exception queue: schedule checks, long shifts, clock divergence,
// device-attested counts, exhausted retries, stale devices, and the
// employee's flag to their approver (which opens a ticket row here).
TableDef exceptions() {
  TableDef t;
  t.name = "exceptions";
  t.columns = {col("id", ColumnType::Integer),
               col("kind", ColumnType::Text),
               col("state", ColumnType::Text),
               col("employee_id", ColumnType::Integer, true),
               col("device_id", ColumnType::Integer, true),
               col("entry_id", ColumnType::Integer, true),
               col("period_id", ColumnType::Integer, true),
               col("detail", ColumnType::Text, true),
               col("opened_at", ColumnType::Timestamp),
               col("resolved_at", ColumnType::Timestamp, true),
               col("resolved_by", ColumnType::Text, true),
               col("resolution_audit_id", ColumnType::Integer, true)};
  t.primary_key = {"id"};
  t.indexes = {index("exceptions_state", {"state", "opened_at"}), index("exceptions_employee", {"employee_id"}),
               index("exceptions_device", {"device_id"}), index("exceptions_entry", {"entry_id"}),
               index("exceptions_period", {"period_id"}), index("exceptions_audit", {"resolution_audit_id"})};
  t.foreign_keys = {fk("exceptions_employee_fk", {"employee_id"}, "employees", {"id"}),
                    fk("exceptions_device_fk", {"device_id"}, "devices", {"id"}),
                    fk("exceptions_entry_fk", {"entry_id"}, "time_entries", {"id"}),
                    fk("exceptions_period_fk", {"period_id"}, "pay_periods", {"id"}),
                    fk("exceptions_audit_fk", {"resolution_audit_id"}, "audit_log", {"id"})};
  t.checks = {one_of("exceptions_kind_known", "kind",
                     {"outside_schedule", "non_scheduled_day", "no_schedule", "long_shift", "clock_divergence",
                      "device_attested_count", "retry_exhausted", "device_stale", "approver_flag"}),
              one_of("exceptions_state_known", "state", {"open", "resolved", "dismissed"})};
  return t;
}

Status apply_v1(Writer& w) {
  for (const TableDef& t : {employees(), role_grants(), local_accounts(), devices(), pay_periods(), schedules(),
                            audit_log(), time_entries(), sync_batches(), approvals(), exceptions()}) {
    if (Status s = w.create_table(t); !s.ok()) return s;
  }
  return Status();
}

}  // namespace

const std::vector<std::string>& v1_tables() {
  static const std::vector<std::string> names = {"employees",   "role_grants",  "local_accounts", "devices",
                                                 "pay_periods", "schedules",    "audit_log",      "time_entries",
                                                 "sync_batches", "approvals",   "exceptions"};
  return names;
}

const std::vector<Migration>& migrations() {
  static const std::vector<Migration> list = {
      Migration{1, "punchline_v1_schema", &apply_v1},
  };
  return list;
}

}  // namespace archivum::punchline
