#include "archivum/punchline/rules.h"

namespace archivum::punchline::rules {
namespace {
using engine::Bound;
using engine::Row;
using engine::Value;
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

}  // namespace archivum::punchline::rules
