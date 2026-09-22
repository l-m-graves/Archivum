// Typed access to the Punchline tables. Column indexes are the declaration
// order in schema.cpp; these structs keep that knowledge in one place. A
// zero timestamp or id means NULL where the column is nullable.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "archivum/engine/store.h"

namespace archivum::punchline {

struct Employee {
  std::int64_t id = 0;
  std::string employee_number, display_name, email, tid, oid, pay_group, site_zone;
  bool active = true;
  std::int64_t created_at = 0, updated_at = 0;
  std::string company, cost_center;  // the old store's routing key; server-owned
  engine::Row to_row() const;
  static Employee from_row(const engine::Row& r);
};

struct Device {
  std::int64_t id = 0;
  engine::UuidBytes device_uuid{};
  std::string name;
  std::int64_t employee_id = 0;
  std::vector<std::byte> credential_hash;
  std::int64_t enrolled_at = 0;
  std::string enrolled_by;
  std::int64_t revoked_at = 0;  // 0: active
  std::string revoked_reason;
  std::int64_t last_seen_at = 0;
  std::int64_t last_acked_sequence = -1;  // -1: none
  std::optional<engine::UuidBytes> last_journal_id;
  std::int64_t employee_id_rejections = 0;
  engine::Row to_row() const;
  static Device from_row(const engine::Row& r);
};

struct PayPeriod {
  std::int64_t id = 0;
  std::int64_t start_day = 0, end_day = 0;  // site-local days since 1970-01-01, inclusive
  std::string site_zone, tzdb_version;
  std::string state = "open";  // open | locked
  std::int64_t submit_by = 0, approve_by = 0, release_at = 0;  // 0: none
  engine::Row to_row() const;
  static PayPeriod from_row(const engine::Row& r);
};

struct SupervisorAssignment {
  std::int64_t id = 0;
  std::int64_t employee_id = 0, supervisor_employee_id = 0;
  std::int64_t effective_from = 0, effective_to = 0;  // [from, to), to == 0: open
  std::string assigned_by;
  std::int64_t audit_id = 0;
  engine::Row to_row() const;
  static SupervisorAssignment from_row(const engine::Row& r);
};

struct Schedule {
  std::int64_t id = 0;
  std::int64_t employee_id = 0;
  int weekday = 0;  // 0 Sunday .. 6 Saturday
  int start_minute = 0, end_minute = 0;
  std::int64_t effective_from_day = 0, effective_to_day = 0;  // [from, to) in local days; to == 0: open
  engine::Row to_row() const;
  static Schedule from_row(const engine::Row& r);
};

struct TimeEntry {
  std::int64_t id = 0;
  engine::UuidBytes entry_uuid{};
  std::int64_t employee_id = 0, device_id = 0;
  std::optional<engine::UuidBytes> journal_id;  // none for a manual entry
  std::int64_t journal_sequence = 0;
  std::string kind;  // in | out
  std::int64_t device_time = 0;
  std::string local_time, site_zone, tzdb_version;
  std::int64_t receipt_time = 0;
  std::string attestation;  // device | server | manual
  std::optional<std::int64_t> clock_divergence_us;
  std::int64_t period_id = 0;  // 0: none
  std::string state = "recorded";
  std::string pay_code;  // empty: none
  std::int64_t approval_audit_id = 0;
  std::int64_t correction_of = 0;
  std::string correction_reason;
  std::int64_t created_at = 0;
  std::string note;
  std::int64_t superseded_by = 0;  // 0: current
  engine::Row to_row() const;
  static TimeEntry from_row(const engine::Row& r);
};

struct Shift {
  std::int64_t id = 0;
  std::int64_t employee_id = 0, device_id = 0;
  std::int64_t in_entry_id = 0, out_entry_id = 0;  // out 0: open
  std::int64_t local_day = 0;
  std::int64_t period_id = 0;
  std::int64_t in_time = 0, out_time = 0;
  std::optional<std::int64_t> duration_us;
  std::string pay_code;
  std::string state = "recorded";
  engine::Row to_row() const;
  static Shift from_row(const engine::Row& r);
};

struct SyncBatch {
  std::int64_t id = 0;
  engine::UuidBytes batch_uuid{};
  std::int64_t device_id = 0;
  std::int64_t received_at = 0;
  std::int64_t first_sequence = 0, last_sequence = 0;
  std::int64_t accepted = 0, rejected = 0;
  std::int64_t client_time_us = 0;
  std::optional<std::int64_t> clock_divergence_us;
  std::optional<engine::UuidBytes> journal_id;
  engine::Row to_row() const;
  static SyncBatch from_row(const engine::Row& r);
};

struct Approval {
  std::int64_t id = 0;
  std::int64_t period_id = 0, employee_id = 0;
  std::string from_state, to_state;
  std::string acted_by_tid, acted_by_oid, acted_by_account;
  std::int64_t acted_at = 0;
  std::string note;
  std::int64_t audit_id = 0;
  engine::Row to_row() const;
  static Approval from_row(const engine::Row& r);
};

struct Exception {
  std::int64_t id = 0;
  std::string kind;
  std::string state = "open";  // open | resolved | dismissed
  std::int64_t employee_id = 0, device_id = 0, entry_id = 0, period_id = 0;  // 0: none
  std::string detail;
  std::int64_t opened_at = 0, resolved_at = 0;
  std::string resolved_by;
  std::int64_t resolution_audit_id = 0;
  engine::Row to_row() const;
  static Exception from_row(const engine::Row& r);
};

Result<std::optional<Employee>> employee_by_id(engine::Reader& r, std::int64_t id);
Result<std::optional<Employee>> employee_by_number(engine::Reader& r, const std::string& number);
Result<std::optional<Employee>> employee_by_identity(engine::Reader& r, const std::string& tid, const std::string& oid);
Result<std::optional<Device>> device_by_uuid(engine::Reader& r, const engine::UuidBytes& uuid);
Result<std::optional<Device>> device_by_id(engine::Reader& r, std::int64_t id);
Result<std::optional<PayPeriod>> period_by_id(engine::Reader& r, std::int64_t id);
// The period whose [start_day, end_day] contains `local_day`, if any.
Result<std::optional<PayPeriod>> period_for_day(engine::Reader& r, std::int64_t local_day);
Result<std::optional<TimeEntry>> entry_by_id(engine::Reader& r, std::int64_t id);
Result<std::optional<TimeEntry>> entry_by_uuid(engine::Reader& r, const engine::UuidBytes& uuid);
Result<std::optional<Shift>> shift_by_id(engine::Reader& r, std::int64_t id);
Result<std::optional<SyncBatch>> batch_by_uuid(engine::Reader& r, const engine::UuidBytes& uuid);
Result<std::optional<Exception>> exception_by_id(engine::Reader& r, std::int64_t id);
// Entries of an employee whose device_time is in [from, to), in time order.
Result<std::vector<TimeEntry>> entries_of(engine::Reader& r, std::int64_t employee_id, std::int64_t from_us, std::int64_t to_us);
Result<std::vector<TimeEntry>> entries_in_period(engine::Reader& r, std::int64_t period_id, std::int64_t employee_id);
Result<std::vector<Shift>> shifts_in_period(engine::Reader& r, std::int64_t period_id, std::int64_t employee_id);
// The employee's open shift (no out entry), if any: the latest by in_time.
Result<std::optional<Shift>> open_shift_of(engine::Reader& r, std::int64_t employee_id);
Result<std::vector<Schedule>> schedules_of(engine::Reader& r, std::int64_t employee_id);
Result<std::vector<SupervisorAssignment>> assignments_of(engine::Reader& r, std::int64_t employee_id);
// Employees whose supervisor at `at_us` is `supervisor_employee_id`.
Result<std::vector<std::int64_t>> reports_of(engine::Reader& r, std::int64_t supervisor_employee_id, std::int64_t at_us);

}  // namespace archivum::punchline
