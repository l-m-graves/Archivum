// The payroll-reference rules (docs/punchline-module.md, "Payroll reference
// rules"; migration 2 of the module): scheduled hours and the flag, the
// overtime authorization that clears it, the annotation, break and pay code
// kinds, the meal premium rule, and the record of who edited a punch and why.
//
// The flag is a HOLD on an employee's timesheet for a period. It is not a
// lifecycle state and it is not an exception: while one is open the
// timesheet is hidden from payroll and cannot be released. The exception
// queue surfaces problems to a reviewer; the hold withholds a timesheet from
// payroll until a supervisor acts. They are separate tables and separate
// code paths on purpose.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "archivum/core/recorder.h"
#include "archivum/punchline/config.h"
#include "archivum/punchline/data.h"

namespace archivum::punchline {

struct ScheduleHours {
  std::int64_t id = 0, employee_id = 0;
  std::int64_t effective_from_day = 0, effective_to_day = 0;  // to: 0 open
  std::int64_t daily_minutes = 0, weekly_minutes = 0;
  std::string set_by;
  std::int64_t audit_id = 0;
  engine::Row to_row() const;
  static ScheduleHours from_row(const engine::Row& r);
};

struct OvertimeAuthorization {
  std::int64_t id = 0, employee_id = 0, period_id = 0;
  std::int64_t authorized_minutes = 0;  // the overtime excess the supervisor saw and authorized
  std::string note, authorized_by_tid, authorized_by_oid;
  std::int64_t authorized_at = 0, audit_id = 0;
  engine::Row to_row() const;
  static OvertimeAuthorization from_row(const engine::Row& r);
};

struct TimesheetHold {
  std::int64_t id = 0, employee_id = 0, period_id = 0;
  std::string reason;  // daily_hours | weekly_hours
  std::string detail;
  std::int64_t opened_at = 0, cleared_at = 0, authorization_id = 0, audit_id = 0;  // cleared_at 0: open
  engine::Row to_row() const;
  static TimesheetHold from_row(const engine::Row& r);
};

struct BreakPremiumRule {
  std::int64_t id = 0;
  std::string break_code;
  std::int64_t threshold_minutes = 0;
  std::string comparison;  // greater_than | at_least
  std::string definition, source;
  bool confirmed = false;
  std::int64_t effective_from_day = 0, effective_to_day = 0;
  std::string set_by;
  std::int64_t audit_id = 0;
  engine::Row to_row() const;
  static BreakPremiumRule from_row(const engine::Row& r);
};

struct MealBreak {
  std::int64_t id = 0, employee_id = 0, out_entry_id = 0, in_entry_id = 0, local_day = 0;
  std::int64_t started_at = 0, ended_at = 0;  // ended_at 0: still on the meal
  std::int64_t minutes = -1;                  // -1: not ended
  bool premium = false;
  std::int64_t premium_rule_id = 0;
  engine::Row to_row() const;
  static MealBreak from_row(const engine::Row& r);
};

struct EntryEdit {
  std::int64_t id = 0, entry_id = 0;
  std::string edit_class;  // payroll | supervisor
  std::string reason_code, editor;
  std::int64_t edited_at = 0, audit_id = 0;
  engine::Row to_row() const;
  static EntryEdit from_row(const engine::Row& r);
};

// ---- scheduled hours -------------------------------------------------------

// The scheduled hours in force on `local_day`, if any.
Result<std::optional<ScheduleHours>> schedule_hours_at(engine::Reader& r, std::int64_t employee_id, std::int64_t local_day);
Result<std::vector<ScheduleHours>> schedule_hours_of(engine::Reader& r, std::int64_t employee_id);
// Adds a row; the effective range must not overlap another row of the employee.
Result<std::int64_t> set_schedule_hours(core::Recorder& rec, std::int64_t employee_id, std::int64_t from_day, std::int64_t to_day,
                                        std::int64_t daily_minutes, std::int64_t weekly_minutes, const std::string& set_by,
                                        std::int64_t now_us);

// ---- the hold --------------------------------------------------------------

Result<std::optional<TimesheetHold>> open_hold(engine::Reader& r, std::int64_t employee_id, std::int64_t period_id);
// Employees with an open hold in the period.
Result<std::set<std::int64_t>> held_employees(engine::Reader& r, std::int64_t period_id);
Result<std::vector<TimesheetHold>> holds_in_period(engine::Reader& r, std::int64_t period_id);

// Sums the employee's closed shifts in the period per local day and per week
// (weeks start on cfg.week_start_weekday) and compares them with the scheduled
// hours in force. Over either limit by more overtime than a supervisor last
// authorized for the period (the excess, not the total), opens (or refreshes) the hold; back within it,
// closes the hold. No scheduled hours on file means nothing to compare with
// and no hold.
Status evaluate_hold(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t period_id, std::int64_t now_us);
// Every period in which the employee has shifts that is still open.
Status evaluate_employee_holds(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t now_us);

// The supervisor's authorization: records the overtime excess now on the
// timesheet as authorized and clears the open hold. NotFound when nothing is
// held. More overtime than that reopens the hold; hours worked within the
// schedule do not.
Result<OvertimeAuthorization> authorize_overtime(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t period_id, const std::string& note,
                                                 const std::string& tid, const std::string& oid, std::int64_t now_us);

// ---- codes -----------------------------------------------------------------

// L (late) and E (early), relative to the schedule in force, recorded against
// the shift. Informational; they never change pay. Replaces the shift's rows.
Status record_annotations(core::Recorder& rec, const Config& cfg, const Shift& shift, const TimeEntry& in, const TimeEntry& out);
// Removes a shift's annotations (before the shift itself is removed).
Status remove_annotations(core::Recorder& rec, std::int64_t shift_id);

Result<bool> break_code_active(engine::Reader& r, const std::string& code);
// A punch marked with a break code (an out punch marked M begins a meal).
Status mark_entry_break(core::Recorder& rec, std::int64_t entry_id, const std::string& code);
// The confirmed premium rule for the code in force on `local_day`. An
// unconfirmed rule is stored and never applied.
Result<std::optional<BreakPremiumRule>> premium_rule_at(engine::Reader& r, const std::string& break_code, std::int64_t local_day);
Result<std::int64_t> add_premium_rule(core::Recorder& rec, const std::string& break_code, std::int64_t threshold_minutes, const std::string& comparison,
                                      const std::string& definition, const std::string& source, bool confirmed, std::int64_t from_day,
                                      std::int64_t to_day, const std::string& set_by, std::int64_t now_us);
// Rebuilds the meals that punches of the employee from `from_us` on began:
// each out punch marked M, through the employee's next punch.
Status rebuild_meals(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t from_us);
Result<std::vector<MealBreak>> meals_in_period(engine::Reader& r, std::int64_t employee_id, std::int64_t period_id);

// ---- edits -----------------------------------------------------------------

Status record_entry_edit(core::Recorder& rec, std::int64_t entry_id, const std::string& edit_class, const std::string& reason_code,
                         const std::string& editor, std::int64_t now_us);
Result<std::vector<EntryEdit>> all_entry_edits(engine::Reader& r);

}  // namespace archivum::punchline
