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
  return CheckDef{name, column, CheckOp::Ne, {Value::text("")}, ""};
}
CheckDef at_least(const char* name, const char* column, std::int64_t v) {
  return CheckDef{name, column, CheckOp::Ge, {Value::integer(v)}, ""};
}
CheckDef at_most(const char* name, const char* column, std::int64_t v) {
  return CheckDef{name, column, CheckOp::Le, {Value::integer(v)}, ""};
}
// Same-row comparison between two columns: an engine check since Stage 6
// (NULL on either side passes).
CheckDef column_after(const char* name, const char* column, const char* other) {
  return CheckDef{name, column, CheckOp::Gt, {}, other};
}
CheckDef column_at_or_after(const char* name, const char* column, const char* other) {
  return CheckDef{name, column, CheckOp::Ge, {}, other};
}

// Lifecycle states of an entry and of a period (punchline-updates.md):
// recorded -> submitted -> approved -> released -> locked.
const std::vector<const char*> kEntryStates = {"recorded", "submitted", "approved", "released", "locked"};
// A period is open or locked; the per-employee lifecycle lives on the
// entries and shifts, with one approvals row per transition.
const std::vector<const char*> kPeriodStates = {"open", "locked"};

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
               col("last_acked_sequence", ColumnType::Integer, true),
               // Stage 6: the client journal's id (its replay key with the
               // sequence), and the count of requests from this device that
               // asserted an employee id (a tamper signal; Stage 6 ruling).
               col("last_journal_id", ColumnType::Uuid, true),
               col("employee_id_rejections", ColumnType::Integer)};
  t.primary_key = {"id"};
  t.indexes = {index("devices_uuid", {"device_uuid"}, true), index("devices_employee", {"employee_id", "revoked_at"})};
  t.foreign_keys = {fk("devices_employee_fk", {"employee_id"}, "employees", {"id"})};
  t.checks = {non_empty("devices_name_nonempty", "name"), at_least("devices_rejections_min", "employee_id_rejections", 0)};
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
  t.checks = {one_of("pay_periods_state_known", "state", kPeriodStates),
              column_at_or_after("pay_periods_days_ordered", "end_day", "start_day"),
              column_at_or_after("pay_periods_cutoffs_ordered", "approve_by", "submit_by")};
  return t;
}

// Who reports to whom, effective-dated: an approval is evaluated against
// the assignment in force at the time of the action, not the current one.
// The supervisor is an employee row (one who holds the supervisor role
// through role_grants on their identity). No overlap per employee is a
// module rule (docs/punchline-schema.md).
TableDef supervisor_assignments() {
  TableDef t;
  t.name = "supervisor_assignments";
  t.columns = {col("id", ColumnType::Integer),
               col("employee_id", ColumnType::Integer),
               col("supervisor_employee_id", ColumnType::Integer),
               col("effective_from", ColumnType::Timestamp),
               col("effective_to", ColumnType::Timestamp, true),
               col("assigned_by", ColumnType::Text),
               col("audit_id", ColumnType::Integer)};
  t.primary_key = {"id"};
  t.indexes = {index("supervisor_assignments_employee", {"employee_id", "effective_from"}),
               index("supervisor_assignments_supervisor", {"supervisor_employee_id", "effective_from"}),
               index("supervisor_assignments_audit", {"audit_id"})};
  t.foreign_keys = {fk("supervisor_assignments_employee_fk", {"employee_id"}, "employees", {"id"}),
                    fk("supervisor_assignments_supervisor_fk", {"supervisor_employee_id"}, "employees", {"id"}),
                    fk("supervisor_assignments_audit_fk", {"audit_id"}, "audit_log", {"id"})};
  t.checks = {non_empty("supervisor_assignments_by_nonempty", "assigned_by"),
              // [effective_from, effective_to): half-open, so adjacent
              // assignments share no day. Ordered here; distinct employees
              // and no overlap are PL-2 (module).
              column_after("supervisor_assignments_range_ordered", "effective_to", "effective_from")};
  return t;
}

// Pay codes as a lookup table so an entry's code is engine-enforced.
// Seeded by the migration; multipliers are configuration, not schema.
TableDef pay_codes() {
  TableDef t;
  t.name = "pay_codes";
  t.columns = {col("code", ColumnType::Text), col("description", ColumnType::Text), col("paid", ColumnType::Boolean),
               col("active", ColumnType::Boolean)};
  t.primary_key = {"code"};
  t.checks = {non_empty("pay_codes_code_nonempty", "code")};
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
              at_least("schedules_end_min", "end_minute", 0), at_most("schedules_end_max", "end_minute", 1440),
              column_after("schedules_minutes_ordered", "end_minute", "start_minute"),
              column_after("schedules_days_ordered", "effective_to_day", "effective_from_day")};
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
               col("journal_id", ColumnType::Uuid, true),  // NULL for a manual entry
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
               col("created_at", ColumnType::Timestamp),
               col("note", ColumnType::Text, true),
               col("superseded_by", ColumnType::Integer, true)};  // the correction that replaced this entry
  t.primary_key = {"id"};
  t.indexes = {index("time_entries_uuid", {"entry_uuid"}, true),
               // (journal id, sequence) is the client journal's replay key
               // (docs/client-journal.md in the Punchline repository): a
               // recreated journal restarts its sequence under a new id.
               index("time_entries_journal_sequence", {"journal_id", "journal_sequence"}, true),
               index("time_entries_device_sequence", {"device_id", "journal_sequence"}),
               index("time_entries_employee_time", {"employee_id", "device_time"}),
               index("time_entries_period", {"period_id", "employee_id"}),
               index("time_entries_state", {"state"}),
               index("time_entries_correction", {"correction_of"}),
               index("time_entries_superseded", {"superseded_by"}),
               index("time_entries_approval", {"approval_audit_id"})};
  t.indexes.push_back(index("time_entries_pay_code", {"pay_code"}));
  t.indexes.push_back(index("time_entries_attestation", {"attestation", "device_time"}));
  t.foreign_keys = {fk("time_entries_employee_fk", {"employee_id"}, "employees", {"id"}),
                    fk("time_entries_device_fk", {"device_id"}, "devices", {"id"}),
                    fk("time_entries_period_fk", {"period_id"}, "pay_periods", {"id"}),
                    fk("time_entries_approval_fk", {"approval_audit_id"}, "audit_log", {"id"}),
                    fk("time_entries_correction_fk", {"correction_of"}, "time_entries", {"id"}),
                    fk("time_entries_superseded_fk", {"superseded_by"}, "time_entries", {"id"}),
                    fk("time_entries_pay_code_fk", {"pay_code"}, "pay_codes", {"code"})};
  t.checks = {one_of("time_entries_kind_known", "kind", {"in", "out"}),
              one_of("time_entries_attestation_known", "attestation", {"device", "server", "manual"}),
              one_of("time_entries_state_known", "state", kEntryStates),
              at_least("time_entries_sequence_min", "journal_sequence", 0)};
  return t;
}

// Shifts: an in punch paired with its out punch (PL-6, Stage 6). A shift
// with no out entry is open; pairing at sync closes it or flags it. The
// local day and the period are the in punch's; duration is derived at
// pairing and the state follows the lifecycle with the entries.
TableDef shifts() {
  TableDef t;
  t.name = "shifts";
  t.columns = {col("id", ColumnType::Integer),
               col("employee_id", ColumnType::Integer),
               col("device_id", ColumnType::Integer),
               col("in_entry_id", ColumnType::Integer),
               col("out_entry_id", ColumnType::Integer, true),
               col("local_day", ColumnType::Integer),
               col("period_id", ColumnType::Integer, true),
               col("in_time", ColumnType::Timestamp),
               col("out_time", ColumnType::Timestamp, true),
               col("duration_us", ColumnType::Integer, true),
               col("pay_code", ColumnType::Text, true),
               col("state", ColumnType::Text)};
  t.primary_key = {"id"};
  t.indexes = {index("shifts_in_entry", {"in_entry_id"}, true),
               index("shifts_out_entry", {"out_entry_id"}, true),
               index("shifts_employee_day", {"employee_id", "local_day"}),
               index("shifts_employee_state", {"employee_id", "state"}),
               index("shifts_period", {"period_id", "employee_id"}),
               index("shifts_device", {"device_id"}),
               index("shifts_pay_code", {"pay_code"})};
  t.foreign_keys = {fk("shifts_employee_fk", {"employee_id"}, "employees", {"id"}),
                    fk("shifts_device_fk", {"device_id"}, "devices", {"id"}),
                    fk("shifts_in_fk", {"in_entry_id"}, "time_entries", {"id"}),
                    fk("shifts_out_fk", {"out_entry_id"}, "time_entries", {"id"}),
                    fk("shifts_period_fk", {"period_id"}, "pay_periods", {"id"}),
                    fk("shifts_pay_code_fk", {"pay_code"}, "pay_codes", {"code"})};
  t.checks = {one_of("shifts_state_known", "state", kEntryStates),
              column_after("shifts_times_ordered", "out_time", "in_time"),
              at_least("shifts_duration_min", "duration_us", 0)};
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
               col("rejected", ColumnType::Integer),
               col("client_time_us", ColumnType::Timestamp, true),
               col("clock_divergence_us", ColumnType::Integer, true),
               col("journal_id", ColumnType::Uuid, true)};
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

// The exception queue: schedule checks, long shifts, unpaired punches,
// clock divergence, device-attested counts, exhausted retries and journal
// recoveries reported by the client, stale devices, a device asserting an
// employee id (tamper signal), entries past a cutoff (escalation), and the
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
                      "device_attested_count", "retry_exhausted", "device_stale", "approver_flag", "unpaired_punch",
                      "employee_id_asserted", "past_cutoff", "journal_recovery"}),
              one_of("exceptions_state_known", "state", {"open", "resolved", "dismissed"})};
  return t;
}

Status apply_v1(Writer& w) {
  // audit_log belongs to the core module and must already exist.
  if (w.catalog().table("audit_log") == nullptr) return Status::invalid_argument("core schema must be applied first");
  for (const TableDef& t : {employees(), devices(), pay_periods(), supervisor_assignments(), pay_codes(), schedules(),
                            time_entries(), shifts(), sync_batches(), approvals(), exceptions()}) {
    if (Status s = w.create_table(t); !s.ok()) return s;
  }
  for (const auto& [code, description] : std::vector<std::pair<const char*, const char*>>{
           {"regular", "Regular hours"}, {"overtime", "Overtime"}, {"double_time", "Double time"},
           {"pto", "Paid time off"}, {"holiday", "Holiday"}}) {
    if (Status s = w.insert("pay_codes", {Value::text(code), Value::text(description), Value::boolean(true), Value::boolean(true)});
        !s.ok()) {
      return s;
    }
  }
  return Status();
}

}  // namespace

const std::vector<std::string>& v1_tables() {
  static const std::vector<std::string> names = {"employees",    "devices",      "pay_periods",  "supervisor_assignments",
                                                 "pay_codes",    "schedules",    "time_entries", "shifts",
                                                 "sync_batches", "approvals",    "exceptions"};
  return names;
}

const std::vector<Migration>& migrations() {
  static const std::vector<Migration> list = {
      Migration{1, "punchline_v1_schema", &apply_v1},
  };
  return list;
}

void Module::extend_policy(core::RecordPolicy& p) const {
  p.allow("employees", {"id", "employee_number", "display_name", "email", "tid", "oid", "active", "pay_group", "site_zone",
                        "created_at", "updated_at"});
  p.allow("devices", {"id", "device_uuid", "name", "employee_id", "enrolled_at", "enrolled_by", "revoked_at", "revoked_reason",
                      "last_seen_at", "last_acked_sequence", "last_journal_id", "employee_id_rejections"});
  p.allow("pay_periods", {"id", "start_day", "end_day", "site_zone", "tzdb_version", "state", "submit_by", "approve_by", "release_at"});
  p.allow("supervisor_assignments", {"id", "employee_id", "supervisor_employee_id", "effective_from", "effective_to", "assigned_by", "audit_id"});
  p.allow("pay_codes", {"code", "description", "paid", "active"});
  p.allow("schedules", {"id", "employee_id", "weekday", "start_minute", "end_minute", "effective_from_day", "effective_to_day"});
  p.allow("time_entries", {"id", "entry_uuid", "employee_id", "device_id", "journal_id", "journal_sequence", "kind", "device_time",
                           "local_time", "site_zone", "tzdb_version", "receipt_time", "attestation", "clock_divergence_us", "period_id",
                           "state", "pay_code", "approval_audit_id", "correction_of", "created_at", "superseded_by"});
  p.allow("shifts", {"id", "employee_id", "device_id", "in_entry_id", "out_entry_id", "local_day", "period_id", "in_time", "out_time",
                     "duration_us", "pay_code", "state"});
  p.allow("sync_batches", {"id", "batch_uuid", "device_id", "received_at", "first_sequence", "last_sequence", "accepted", "rejected",
                           "client_time_us", "clock_divergence_us", "journal_id"});
  p.allow("approvals", {"id", "period_id", "employee_id", "from_state", "to_state", "acted_by_tid", "acted_by_oid", "acted_by_account",
                        "acted_at", "audit_id"});
  p.allow("exceptions", {"id", "kind", "state", "employee_id", "device_id", "entry_id", "period_id", "opened_at", "resolved_at",
                         "resolved_by", "resolution_audit_id"});
}

const Module& module() {
  static const Module m;
  return m;
}

}  // namespace archivum::punchline
