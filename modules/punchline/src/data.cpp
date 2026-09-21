#include "archivum/punchline/data.h"

#include <algorithm>

namespace archivum::punchline {
namespace {
using engine::Bound;
using engine::Row;
using engine::Value;

Value opt_text(const std::string& s) { return s.empty() ? Value::null() : Value::text(s); }
Value opt_ts(std::int64_t t) { return t == 0 ? Value::null() : Value::timestamp(t); }
Value opt_id(std::int64_t t) { return t == 0 ? Value::null() : Value::integer(t); }
Value opt_int(const std::optional<std::int64_t>& v) { return v ? Value::integer(*v) : Value::null(); }
Value opt_uuid(const std::optional<engine::UuidBytes>& u) { return u ? Value::uuid(*u) : Value::null(); }
std::string text_or_empty(const Value& v) { return v.is_null() ? std::string() : v.as_text(); }
std::int64_t int_or_zero(const Value& v) { return v.is_null() ? 0 : v.as_int64(); }
std::optional<std::int64_t> int_opt(const Value& v) {
  if (v.is_null()) return std::nullopt;
  return v.as_int64();
}
std::optional<engine::UuidBytes> uuid_opt(const Value& v) {
  if (v.is_null()) return std::nullopt;
  return v.as_uuid();
}

template <class T>
Result<std::optional<T>> first(Result<std::vector<Row>> rows) {
  if (!rows.ok()) return rows.status();
  if (rows.value().empty()) return std::optional<T>();
  return std::optional<T>(T::from_row(rows.value()[0]));
}
template <class T>
Result<std::optional<T>> by_pk(engine::Reader& r, const char* table, std::int64_t id) {
  auto row = r.get(table, {Value::integer(id)});
  if (!row.ok()) return row.status();
  if (!row.value().has_value()) return std::optional<T>();
  return std::optional<T>(T::from_row(*row.value()));
}
template <class T>
Result<std::vector<T>> all(Result<std::vector<Row>> rows) {
  if (!rows.ok()) return rows.status();
  std::vector<T> out;
  out.reserve(rows.value().size());
  for (const Row& r : rows.value()) out.push_back(T::from_row(r));
  return out;
}
}  // namespace

Row Employee::to_row() const {
  return {Value::integer(id), Value::text(employee_number), Value::text(display_name), opt_text(email), opt_text(tid),
          opt_text(oid), Value::boolean(active), opt_text(pay_group), Value::text(site_zone), Value::timestamp(created_at),
          Value::timestamp(updated_at)};
}
Employee Employee::from_row(const Row& r) {
  Employee e;
  e.id = r[0].as_int64();
  e.employee_number = r[1].as_text();
  e.display_name = r[2].as_text();
  e.email = text_or_empty(r[3]);
  e.tid = text_or_empty(r[4]);
  e.oid = text_or_empty(r[5]);
  e.active = r[6].as_bool();
  e.pay_group = text_or_empty(r[7]);
  e.site_zone = r[8].as_text();
  e.created_at = r[9].as_int64();
  e.updated_at = r[10].as_int64();
  return e;
}

Row Device::to_row() const {
  return {Value::integer(id), Value::uuid(device_uuid), Value::text(name), Value::integer(employee_id),
          Value::blob(credential_hash), Value::timestamp(enrolled_at), Value::text(enrolled_by), opt_ts(revoked_at),
          opt_text(revoked_reason), opt_ts(last_seen_at),
          last_acked_sequence < 0 ? Value::null() : Value::integer(last_acked_sequence), opt_uuid(last_journal_id),
          Value::integer(employee_id_rejections)};
}
Device Device::from_row(const Row& r) {
  Device d;
  d.id = r[0].as_int64();
  d.device_uuid = r[1].as_uuid();
  d.name = r[2].as_text();
  d.employee_id = r[3].as_int64();
  d.credential_hash = r[4].as_blob();
  d.enrolled_at = r[5].as_int64();
  d.enrolled_by = r[6].as_text();
  d.revoked_at = int_or_zero(r[7]);
  d.revoked_reason = text_or_empty(r[8]);
  d.last_seen_at = int_or_zero(r[9]);
  d.last_acked_sequence = r[10].is_null() ? -1 : r[10].as_int64();
  d.last_journal_id = uuid_opt(r[11]);
  d.employee_id_rejections = r[12].as_int64();
  return d;
}

Row PayPeriod::to_row() const {
  return {Value::integer(id), Value::integer(start_day), Value::integer(end_day), Value::text(site_zone), Value::text(tzdb_version),
          Value::text(state), opt_ts(submit_by), opt_ts(approve_by), opt_ts(release_at)};
}
PayPeriod PayPeriod::from_row(const Row& r) {
  PayPeriod p;
  p.id = r[0].as_int64();
  p.start_day = r[1].as_int64();
  p.end_day = r[2].as_int64();
  p.site_zone = r[3].as_text();
  p.tzdb_version = r[4].as_text();
  p.state = r[5].as_text();
  p.submit_by = int_or_zero(r[6]);
  p.approve_by = int_or_zero(r[7]);
  p.release_at = int_or_zero(r[8]);
  return p;
}

Row SupervisorAssignment::to_row() const {
  return {Value::integer(id), Value::integer(employee_id), Value::integer(supervisor_employee_id), Value::timestamp(effective_from),
          opt_ts(effective_to), Value::text(assigned_by), Value::integer(audit_id)};
}
SupervisorAssignment SupervisorAssignment::from_row(const Row& r) {
  SupervisorAssignment a;
  a.id = r[0].as_int64();
  a.employee_id = r[1].as_int64();
  a.supervisor_employee_id = r[2].as_int64();
  a.effective_from = r[3].as_int64();
  a.effective_to = int_or_zero(r[4]);
  a.assigned_by = r[5].as_text();
  a.audit_id = r[6].as_int64();
  return a;
}

Row Schedule::to_row() const {
  return {Value::integer(id), Value::integer(employee_id), Value::integer(weekday), Value::integer(start_minute), Value::integer(end_minute),
          Value::integer(effective_from_day), effective_to_day == 0 ? Value::null() : Value::integer(effective_to_day)};
}
Schedule Schedule::from_row(const Row& r) {
  Schedule s;
  s.id = r[0].as_int64();
  s.employee_id = r[1].as_int64();
  s.weekday = static_cast<int>(r[2].as_int64());
  s.start_minute = static_cast<int>(r[3].as_int64());
  s.end_minute = static_cast<int>(r[4].as_int64());
  s.effective_from_day = r[5].as_int64();
  s.effective_to_day = int_or_zero(r[6]);
  return s;
}

Row TimeEntry::to_row() const {
  return {Value::integer(id), Value::uuid(entry_uuid), Value::integer(employee_id), Value::integer(device_id), opt_uuid(journal_id),
          Value::integer(journal_sequence), Value::text(kind), Value::timestamp(device_time), Value::text(local_time),
          Value::text(site_zone), Value::text(tzdb_version), Value::timestamp(receipt_time), Value::text(attestation),
          opt_int(clock_divergence_us), opt_id(period_id), Value::text(state), opt_text(pay_code), opt_id(approval_audit_id),
          opt_id(correction_of), opt_text(correction_reason), Value::timestamp(created_at), opt_text(note), opt_id(superseded_by)};
}
TimeEntry TimeEntry::from_row(const Row& r) {
  TimeEntry e;
  e.id = r[0].as_int64();
  e.entry_uuid = r[1].as_uuid();
  e.employee_id = r[2].as_int64();
  e.device_id = r[3].as_int64();
  e.journal_id = uuid_opt(r[4]);
  e.journal_sequence = r[5].as_int64();
  e.kind = r[6].as_text();
  e.device_time = r[7].as_int64();
  e.local_time = r[8].as_text();
  e.site_zone = r[9].as_text();
  e.tzdb_version = r[10].as_text();
  e.receipt_time = r[11].as_int64();
  e.attestation = r[12].as_text();
  e.clock_divergence_us = int_opt(r[13]);
  e.period_id = int_or_zero(r[14]);
  e.state = r[15].as_text();
  e.pay_code = text_or_empty(r[16]);
  e.approval_audit_id = int_or_zero(r[17]);
  e.correction_of = int_or_zero(r[18]);
  e.correction_reason = text_or_empty(r[19]);
  e.created_at = r[20].as_int64();
  e.note = text_or_empty(r[21]);
  e.superseded_by = int_or_zero(r[22]);
  return e;
}

Row Shift::to_row() const {
  return {Value::integer(id), Value::integer(employee_id), Value::integer(device_id), Value::integer(in_entry_id), opt_id(out_entry_id),
          Value::integer(local_day), opt_id(period_id), Value::timestamp(in_time), opt_ts(out_time), opt_int(duration_us),
          opt_text(pay_code), Value::text(state)};
}
Shift Shift::from_row(const Row& r) {
  Shift s;
  s.id = r[0].as_int64();
  s.employee_id = r[1].as_int64();
  s.device_id = r[2].as_int64();
  s.in_entry_id = r[3].as_int64();
  s.out_entry_id = int_or_zero(r[4]);
  s.local_day = r[5].as_int64();
  s.period_id = int_or_zero(r[6]);
  s.in_time = r[7].as_int64();
  s.out_time = int_or_zero(r[8]);
  s.duration_us = int_opt(r[9]);
  s.pay_code = text_or_empty(r[10]);
  s.state = r[11].as_text();
  return s;
}

Row SyncBatch::to_row() const {
  return {Value::integer(id), Value::uuid(batch_uuid), Value::integer(device_id), Value::timestamp(received_at),
          Value::integer(first_sequence), Value::integer(last_sequence), Value::integer(accepted), Value::integer(rejected),
          opt_ts(client_time_us), opt_int(clock_divergence_us), opt_uuid(journal_id)};
}
SyncBatch SyncBatch::from_row(const Row& r) {
  SyncBatch b;
  b.id = r[0].as_int64();
  b.batch_uuid = r[1].as_uuid();
  b.device_id = r[2].as_int64();
  b.received_at = r[3].as_int64();
  b.first_sequence = r[4].as_int64();
  b.last_sequence = r[5].as_int64();
  b.accepted = r[6].as_int64();
  b.rejected = r[7].as_int64();
  b.client_time_us = int_or_zero(r[8]);
  b.clock_divergence_us = int_opt(r[9]);
  b.journal_id = uuid_opt(r[10]);
  return b;
}

Row Approval::to_row() const {
  return {Value::integer(id), Value::integer(period_id), Value::integer(employee_id), Value::text(from_state), Value::text(to_state),
          opt_text(acted_by_tid), opt_text(acted_by_oid), opt_text(acted_by_account), Value::timestamp(acted_at), opt_text(note),
          Value::integer(audit_id)};
}
Approval Approval::from_row(const Row& r) {
  Approval a;
  a.id = r[0].as_int64();
  a.period_id = r[1].as_int64();
  a.employee_id = r[2].as_int64();
  a.from_state = r[3].as_text();
  a.to_state = r[4].as_text();
  a.acted_by_tid = text_or_empty(r[5]);
  a.acted_by_oid = text_or_empty(r[6]);
  a.acted_by_account = text_or_empty(r[7]);
  a.acted_at = r[8].as_int64();
  a.note = text_or_empty(r[9]);
  a.audit_id = r[10].as_int64();
  return a;
}

Row Exception::to_row() const {
  return {Value::integer(id), Value::text(kind), Value::text(state), opt_id(employee_id), opt_id(device_id), opt_id(entry_id),
          opt_id(period_id), opt_text(detail), Value::timestamp(opened_at), opt_ts(resolved_at), opt_text(resolved_by),
          opt_id(resolution_audit_id)};
}
Exception Exception::from_row(const Row& r) {
  Exception x;
  x.id = r[0].as_int64();
  x.kind = r[1].as_text();
  x.state = r[2].as_text();
  x.employee_id = int_or_zero(r[3]);
  x.device_id = int_or_zero(r[4]);
  x.entry_id = int_or_zero(r[5]);
  x.period_id = int_or_zero(r[6]);
  x.detail = text_or_empty(r[7]);
  x.opened_at = r[8].as_int64();
  x.resolved_at = int_or_zero(r[9]);
  x.resolved_by = text_or_empty(r[10]);
  x.resolution_audit_id = int_or_zero(r[11]);
  return x;
}

Result<std::optional<Employee>> employee_by_id(engine::Reader& r, std::int64_t id) { return by_pk<Employee>(r, "employees", id); }
Result<std::optional<Employee>> employee_by_number(engine::Reader& r, const std::string& number) {
  return first<Employee>(r.scan_all("employees", "employees_number", Bound{{Value::text(number)}}, Bound{{Value::text(number)}}));
}
Result<std::optional<Employee>> employee_by_identity(engine::Reader& r, const std::string& tid, const std::string& oid) {
  if (tid.empty() || oid.empty()) return std::optional<Employee>();
  return first<Employee>(r.scan_all("employees", "employees_identity", Bound{{Value::text(tid), Value::text(oid)}},
                                    Bound{{Value::text(tid), Value::text(oid)}}));
}
Result<std::optional<Device>> device_by_uuid(engine::Reader& r, const engine::UuidBytes& uuid) {
  return first<Device>(r.scan_all("devices", "devices_uuid", Bound{{Value::uuid(uuid)}}, Bound{{Value::uuid(uuid)}}));
}
Result<std::optional<Device>> device_by_id(engine::Reader& r, std::int64_t id) { return by_pk<Device>(r, "devices", id); }
Result<std::optional<PayPeriod>> period_by_id(engine::Reader& r, std::int64_t id) { return by_pk<PayPeriod>(r, "pay_periods", id); }
Result<std::optional<PayPeriod>> period_for_day(engine::Reader& r, std::int64_t local_day) {
  // The last period starting at or before the day; it contains the day if its end reaches it.
  std::optional<PayPeriod> found;
  Status s = r.scan("pay_periods", "pay_periods_start", std::nullopt, Bound{{Value::integer(local_day)}}, true, [&](const Row& row) {
    PayPeriod p = PayPeriod::from_row(row);
    if (p.end_day >= local_day) found = p;
    return false;
  });
  if (!s.ok()) return s;
  return found;
}
Result<std::optional<TimeEntry>> entry_by_id(engine::Reader& r, std::int64_t id) { return by_pk<TimeEntry>(r, "time_entries", id); }
Result<std::optional<TimeEntry>> entry_by_uuid(engine::Reader& r, const engine::UuidBytes& uuid) {
  return first<TimeEntry>(r.scan_all("time_entries", "time_entries_uuid", Bound{{Value::uuid(uuid)}}, Bound{{Value::uuid(uuid)}}));
}
Result<std::optional<Shift>> shift_by_id(engine::Reader& r, std::int64_t id) { return by_pk<Shift>(r, "shifts", id); }
Result<std::optional<SyncBatch>> batch_by_uuid(engine::Reader& r, const engine::UuidBytes& uuid) {
  return first<SyncBatch>(r.scan_all("sync_batches", "sync_batches_uuid", Bound{{Value::uuid(uuid)}}, Bound{{Value::uuid(uuid)}}));
}
Result<std::optional<Exception>> exception_by_id(engine::Reader& r, std::int64_t id) { return by_pk<Exception>(r, "exceptions", id); }

Result<std::vector<TimeEntry>> entries_of(engine::Reader& r, std::int64_t employee_id, std::int64_t from_us, std::int64_t to_us) {
  std::vector<TimeEntry> out;
  Status s = r.scan("time_entries", "time_entries_employee_time", Bound{{Value::integer(employee_id), Value::timestamp(from_us)}},
                    Bound{{Value::integer(employee_id), Value::timestamp(to_us)}}, false, [&](const Row& row) {
                      TimeEntry e = TimeEntry::from_row(row);
                      if (e.device_time < to_us) out.push_back(std::move(e));
                      return true;
                    });
  if (!s.ok()) return s;
  return out;
}
Result<std::vector<TimeEntry>> entries_in_period(engine::Reader& r, std::int64_t period_id, std::int64_t employee_id) {
  auto rows = all<TimeEntry>(r.scan_all("time_entries", "time_entries_period", Bound{{Value::integer(period_id), Value::integer(employee_id)}},
                                        Bound{{Value::integer(period_id), Value::integer(employee_id)}}));
  if (!rows.ok()) return rows.status();
  std::sort(rows.value().begin(), rows.value().end(), [](const TimeEntry& a, const TimeEntry& b) {
    return a.device_time != b.device_time ? a.device_time < b.device_time : a.id < b.id;
  });
  return rows;
}
Result<std::vector<Shift>> shifts_in_period(engine::Reader& r, std::int64_t period_id, std::int64_t employee_id) {
  auto rows = all<Shift>(r.scan_all("shifts", "shifts_period", Bound{{Value::integer(period_id), Value::integer(employee_id)}},
                                    Bound{{Value::integer(period_id), Value::integer(employee_id)}}));
  if (!rows.ok()) return rows.status();
  std::sort(rows.value().begin(), rows.value().end(), [](const Shift& a, const Shift& b) { return a.in_time < b.in_time; });
  return rows;
}
Result<std::optional<Shift>> open_shift_of(engine::Reader& r, std::int64_t employee_id) {
  // Open shifts are few: scan the employee's shifts by state and keep the latest.
  std::optional<Shift> latest;
  for (const char* state : {"recorded", "submitted", "approved", "released", "locked"}) {
    Status s = r.scan("shifts", "shifts_employee_state", Bound{{Value::integer(employee_id), Value::text(state)}},
                      Bound{{Value::integer(employee_id), Value::text(state)}}, false, [&](const Row& row) {
                        Shift sh = Shift::from_row(row);
                        if (sh.out_entry_id == 0 && (!latest || sh.in_time > latest->in_time)) latest = sh;
                        return true;
                      });
    if (!s.ok()) return s;
  }
  return latest;
}
Result<std::vector<Schedule>> schedules_of(engine::Reader& r, std::int64_t employee_id) {
  return all<Schedule>(r.scan_all("schedules", "schedules_employee", Bound{{Value::integer(employee_id)}}, Bound{{Value::integer(employee_id)}}));
}
Result<std::vector<SupervisorAssignment>> assignments_of(engine::Reader& r, std::int64_t employee_id) {
  return all<SupervisorAssignment>(r.scan_all("supervisor_assignments", "supervisor_assignments_employee", Bound{{Value::integer(employee_id)}},
                                              Bound{{Value::integer(employee_id)}}));
}
Result<std::vector<std::int64_t>> reports_of(engine::Reader& r, std::int64_t supervisor_employee_id, std::int64_t at_us) {
  std::vector<std::int64_t> out;
  Status s = r.scan("supervisor_assignments", "supervisor_assignments_supervisor", Bound{{Value::integer(supervisor_employee_id)}},
                    Bound{{Value::integer(supervisor_employee_id)}}, false, [&](const Row& row) {
                      SupervisorAssignment a = SupervisorAssignment::from_row(row);
                      if (a.effective_from <= at_us && (a.effective_to == 0 || at_us < a.effective_to)) out.push_back(a.employee_id);
                      return true;
                    });
  if (!s.ok()) return s;
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace archivum::punchline
