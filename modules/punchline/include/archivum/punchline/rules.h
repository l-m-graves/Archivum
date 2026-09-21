// Module-enforced invariants: rules the engine's constraints cannot
// express (docs/punchline-schema.md, "Enforced by the module"). Each is a
// function the request handlers call before mutating, and each has a test
// asserting that the engine accepts the bad row and the module rejects it.
// Every one returns ErrorCode::Constraint with the rule's name.
//
// Same-row comparisons between two columns are engine checks since Stage
// 6 (`supervisor_assignments_range_ordered`, `schedules_minutes_ordered`,
// `pay_periods_days_ordered`, `shifts_times_ordered`); what remains here
// relates two rows or a row to the caller.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include "archivum/engine/store.h"
#include "archivum/punchline/data.h"

namespace archivum::punchline::rules {

// PL-1: at most one unrevoked device per employee. `except_device` is the
// device being updated, if any.
Status one_active_device(engine::Reader& reader, std::int64_t employee_id, std::int64_t except_device = 0);

// PL-2: a supervisor assignment names two different employees and does
// not overlap another assignment of the same employee. Ranges are
// half-open, [effective_from, effective_to), effective_to 0 meaning open,
// so two assignments may meet at an instant and no day belongs to two
// supervisors. (Ordering of the range itself is the engine's check.)
Status supervisor_assignment_valid(engine::Reader& reader, std::int64_t employee_id, std::int64_t supervisor_employee_id,
                                   std::int64_t effective_from, std::int64_t effective_to /*0: open*/,
                                   std::int64_t except_assignment = 0);

// The supervisor of `employee_id` in force at `at_us` under the half-open
// rule, if any.
Result<std::optional<std::int64_t>> supervisor_at(engine::Reader& reader, std::int64_t employee_id, std::int64_t at_us);

// PL-3: an entry's period, when set, is the period containing its local
// day (site-local days, from the wall clock the device recorded).
Status entry_period_valid(engine::Reader& reader, std::int64_t local_day, std::int64_t period_id /*0: none*/);

// PL-4: a correction is of an existing entry of the same employee and
// carries a reason.
Status correction_valid(engine::Reader& reader, std::int64_t original_entry_id, std::int64_t employee_id,
                        const std::string& reason);

// PL-5: a device-attested entry comes from a device that was unrevoked at
// its receipt time.
Status device_attested_valid(engine::Reader& reader, std::int64_t device_id, std::int64_t receipt_time_us);

// PL-6: an out punch closes an in punch of the same employee and device
// and comes after it; a shift longer than `long_shift_us` is not refused
// but must be flagged (the caller opens `long_shift`). `pair_out` returns
// Constraint when the out punch cannot close `open` (wrong device, or
// not after the in punch).
Status pair_out(const Shift& open, const TimeEntry& out_punch);

// PL-7: lifecycle transitions in order only (recorded -> submitted ->
// approved -> released -> locked) by a caller holding the standing the
// transition needs, evaluated against the supervisor assignment in force
// at `acted_at`:
//   submit   the employee themself, or their supervisor at acted_at, or payroll
//   approve  their supervisor at acted_at (role supervisor), or payroll
//            with a non-empty override reason (escalation)
//   release  payroll
//   lock     payroll or admin
struct Standing {
  std::set<std::string> roles;
  std::optional<std::int64_t> employee_id;  // the caller's own employee row, if any
  std::string override_reason;              // payroll approving in place of the supervisor
};
const char* next_state(const std::string& from);  // "" when from is locked or unknown
Status transition_allowed(engine::Reader& reader, std::int64_t employee_id, const std::string& from, const std::string& to,
                          const Standing& who, std::int64_t acted_at);

}  // namespace archivum::punchline::rules
