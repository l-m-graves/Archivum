#include "archivum/punchline/rules.h"

namespace archivum::punchline::rules {
namespace {
using engine::Bound;
using engine::Row;
using engine::Value;

bool holds(const Standing& who, const char* role) { return who.roles.count(role) != 0; }
}  // namespace

Status one_active_device(engine::Reader& reader, std::int64_t employee_id, std::int64_t except_device) {
  bool active = false;
  Status s = reader.scan("devices", "devices_employee", Bound{{Value::integer(employee_id)}},
                         Bound{{Value::integer(employee_id)}}, false, [&](const Row& r) {
                           if (r[0].as_int64() == except_device) return true;
                           if (r[7].is_null()) {  // revoked_at
                             active = true;
                             return false;
                           }
                           return true;
                         });
  if (!s.ok()) return s;
  if (active) return Status::constraint("PL-1: employee already has an active device");
  return Status();
}

Status supervisor_assignment_valid(engine::Reader& reader, std::int64_t employee_id, std::int64_t supervisor_employee_id,
                                   std::int64_t effective_from, std::int64_t effective_to, std::int64_t except_assignment) {
  if (employee_id == supervisor_employee_id) return Status::constraint("PL-2: an employee cannot supervise themself");
  if (effective_to != 0 && effective_to <= effective_from) return Status::constraint("PL-2: effective_to must be after effective_from");
  bool overlap = false;
  Status s = reader.scan("supervisor_assignments", "supervisor_assignments_employee", Bound{{Value::integer(employee_id)}},
                         Bound{{Value::integer(employee_id)}}, false, [&](const Row& r) {
                           if (r[0].as_int64() == except_assignment) return true;
                           const std::int64_t from = r[3].as_int64();
                           const std::int64_t to = r[4].is_null() ? 0 : r[4].as_int64();
                           // Half-open on both sides: [a, b) and [b, c) do not overlap.
                           const bool before = effective_to != 0 && effective_to <= from;
                           const bool after = to != 0 && to <= effective_from;
                           if (!before && !after) {
                             overlap = true;
                             return false;
                           }
                           return true;
                         });
  if (!s.ok()) return s;
  if (overlap) return Status::constraint("PL-2: overlaps another assignment of this employee");
  return Status();
}

Result<std::optional<std::int64_t>> supervisor_at(engine::Reader& reader, std::int64_t employee_id, std::int64_t at_us) {
  std::optional<std::int64_t> found;
  Status s = reader.scan("supervisor_assignments", "supervisor_assignments_employee", Bound{{Value::integer(employee_id)}},
                         Bound{{Value::integer(employee_id)}}, false, [&](const Row& r) {
                           const std::int64_t from = r[3].as_int64();
                           const std::int64_t to = r[4].is_null() ? 0 : r[4].as_int64();
                           if (from <= at_us && (to == 0 || at_us < to)) {
                             found = r[2].as_int64();
                             return false;
                           }
                           return true;
                         });
  if (!s.ok()) return s;
  return found;
}

Status entry_period_valid(engine::Reader& reader, std::int64_t local_day, std::int64_t period_id) {
  if (period_id == 0) return Status();
  auto p = period_by_id(reader, period_id);
  if (!p.ok()) return p.status();
  if (!p.value().has_value()) return Status::constraint("PL-3: no such pay period");
  if (local_day < p.value()->start_day || local_day > p.value()->end_day) {
    return Status::constraint("PL-3: entry's local day is outside its pay period");
  }
  return Status();
}

Status correction_valid(engine::Reader& reader, std::int64_t original_entry_id, std::int64_t employee_id, const std::string& reason) {
  if (reason.empty()) return Status::constraint("PL-4: a correction needs a reason");
  auto e = entry_by_id(reader, original_entry_id);
  if (!e.ok()) return e.status();
  if (!e.value().has_value()) return Status::constraint("PL-4: no such entry to correct");
  if (e.value()->employee_id != employee_id) return Status::constraint("PL-4: a correction must be of the same employee's entry");
  return Status();
}

Status device_attested_valid(engine::Reader& reader, std::int64_t device_id, std::int64_t receipt_time_us) {
  auto d = device_by_id(reader, device_id);
  if (!d.ok()) return d.status();
  if (!d.value().has_value()) return Status::constraint("PL-5: no such device");
  if (d.value()->revoked_at != 0 && d.value()->revoked_at <= receipt_time_us) {
    return Status::constraint("PL-5: device was revoked before the entry was received");
  }
  return Status();
}

Status pair_out(const Shift& open, const TimeEntry& out_punch) {
  if (out_punch.kind != "out") return Status::constraint("PL-6: not an out punch");
  if (out_punch.employee_id != open.employee_id) return Status::constraint("PL-6: out punch is another employee's");
  if (out_punch.device_id != open.device_id) return Status::constraint("PL-6: out punch is from another device");
  if (out_punch.device_time <= open.in_time) return Status::constraint("PL-6: out punch is not after the in punch");
  return Status();
}

const char* next_state(const std::string& from) {
  if (from == "recorded") return "submitted";
  if (from == "submitted") return "approved";
  if (from == "approved") return "released";
  if (from == "released") return "locked";
  return "";
}

Status transition_allowed(engine::Reader& reader, std::int64_t employee_id, const std::string& from, const std::string& to,
                          const Standing& who, std::int64_t acted_at) {
  if (std::string(next_state(from)) != to || to.empty()) {
    return Status::constraint("PL-7: transition " + from + " -> " + to + " is out of order");
  }
  if (to == "submitted") {
    if (who.employee_id && *who.employee_id == employee_id) return Status();
    if (holds(who, "payroll")) return Status();
    if (holds(who, "supervisor") && who.employee_id) {
      auto sup = supervisor_at(reader, employee_id, acted_at);
      if (!sup.ok()) return sup.status();
      if (sup.value() && *sup.value() == *who.employee_id) return Status();
    }
    return Status::constraint("PL-7: only the employee, their supervisor, or payroll may submit");
  }
  if (to == "approved") {
    if (holds(who, "supervisor") && who.employee_id) {
      auto sup = supervisor_at(reader, employee_id, acted_at);
      if (!sup.ok()) return sup.status();
      if (sup.value() && *sup.value() == *who.employee_id) return Status();
    }
    if (holds(who, "payroll")) {
      if (who.override_reason.empty()) return Status::constraint("PL-7: payroll approving in place of the supervisor needs an override reason");
      return Status();
    }
    return Status::constraint("PL-7: only the supervisor in force at the time of the action may approve");
  }
  if (to == "released") {
    if (holds(who, "payroll")) return Status();
    return Status::constraint("PL-7: only payroll may release");
  }
  if (to == "locked") {
    if (holds(who, "payroll") || holds(who, "admin")) return Status();
    return Status::constraint("PL-7: only payroll or admin may lock");
  }
  return Status::constraint("PL-7: unknown transition");
}

}  // namespace archivum::punchline::rules
