#include "archivum/punchline/payroll.h"

#include <algorithm>
#include <limits>
#include <map>

#include "archivum/core/ids.h"
#include "archivum/punchline/localtime.h"

namespace archivum::punchline {
namespace {

using engine::Bound;
using engine::Row;
using engine::Value;

constexpr std::int64_t kUsPerMinute = 60'000'000;

std::int64_t int_or_zero(const Value& v) { return v.is_null() ? 0 : v.as_int64(); }
Value int_or_null(std::int64_t v) { return v == 0 ? Value::null() : Value::integer(v); }

template <class T>
Result<std::vector<T>> all(Result<std::vector<Row>> rows) {
  if (!rows.ok()) return rows.status();
  std::vector<T> out;
  out.reserve(rows.value().size());
  for (const Row& r : rows.value()) out.push_back(T::from_row(r));
  return out;
}

std::string hours_minutes(std::int64_t minutes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lldh%02lld", static_cast<long long>(minutes / 60), static_cast<long long>(minutes % 60));
  return buf;
}

// Closed-shift minutes of the employee in the period, in total, per local day
// and per week.
struct Totals {
  std::int64_t total_us = 0;
  std::map<std::int64_t, std::int64_t> day_us, week_us;
};

Result<Totals> totals_of(engine::Reader& r, const Config& cfg, std::int64_t employee_id, std::int64_t period_id) {
  auto shifts = shifts_in_period(r, period_id, employee_id);
  if (!shifts.ok()) return shifts.status();
  Totals t;
  for (const Shift& sh : shifts.value()) {
    if (sh.out_entry_id == 0 || !sh.duration_us) continue;
    const std::int64_t us = *sh.duration_us;
    t.total_us += us;
    t.day_us[sh.local_day] += us;
    const std::int64_t back = ((weekday_of_day(sh.local_day) - cfg.week_start_weekday) % 7 + 7) % 7;
    t.week_us[sh.local_day - back] += us;
  }
  return t;
}

Result<std::optional<OvertimeAuthorization>> latest_authorization(engine::Reader& r, std::int64_t employee_id, std::int64_t period_id) {
  auto rows = all<OvertimeAuthorization>(r.scan_all("overtime_authorizations", "overtime_authorizations_timesheet",
                                                    Bound{{Value::integer(employee_id), Value::integer(period_id)}},
                                                    Bound{{Value::integer(employee_id), Value::integer(period_id)}}));
  if (!rows.ok()) return rows.status();
  if (rows.value().empty()) return std::optional<OvertimeAuthorization>();
  return std::optional<OvertimeAuthorization>(rows.value().back());  // ids ascend
}

// The overtime a reviewer is asked to authorize: minutes over the scheduled
// daily limit summed over the days, or over the weekly limit summed over the
// weeks, whichever is larger. Authorizing records this figure; a later day
// worked within its schedule adds nothing to it and so never reopens the hold.
Result<std::int64_t> overtime_excess_minutes(engine::Reader& r, std::int64_t employee_id, const Totals& totals) {
  std::int64_t daily = 0, weekly = 0;
  for (const auto& [day, us] : totals.day_us) {
    auto hours = schedule_hours_at(r, employee_id, day);
    if (!hours.ok()) return hours.status();
    if (hours.value()) daily += std::max<std::int64_t>(0, us / kUsPerMinute - hours.value()->daily_minutes);
  }
  for (const auto& [week_start, us] : totals.week_us) {
    auto hours = schedule_hours_at(r, employee_id, week_start);
    if (!hours.ok()) return hours.status();
    if (hours.value()) weekly += std::max<std::int64_t>(0, us / kUsPerMinute - hours.value()->weekly_minutes);
  }
  return std::max(daily, weekly);
}

}  // namespace

// ---- rows ------------------------------------------------------------------

Row ScheduleHours::to_row() const {
  return {Value::integer(id), Value::integer(employee_id), Value::integer(effective_from_day), int_or_null(effective_to_day),
          Value::integer(daily_minutes), Value::integer(weekly_minutes), Value::text(set_by), Value::integer(audit_id)};
}
ScheduleHours ScheduleHours::from_row(const Row& r) {
  ScheduleHours s;
  s.id = r[0].as_int64();
  s.employee_id = r[1].as_int64();
  s.effective_from_day = r[2].as_int64();
  s.effective_to_day = int_or_zero(r[3]);
  s.daily_minutes = r[4].as_int64();
  s.weekly_minutes = r[5].as_int64();
  s.set_by = r[6].as_text();
  s.audit_id = r[7].as_int64();
  return s;
}

Row OvertimeAuthorization::to_row() const {
  return {Value::integer(id),          Value::integer(employee_id),      Value::integer(period_id),        Value::integer(authorized_minutes),
          Value::text(note),           Value::text(authorized_by_tid),   Value::text(authorized_by_oid),   Value::timestamp(authorized_at),
          Value::integer(audit_id)};
}
OvertimeAuthorization OvertimeAuthorization::from_row(const Row& r) {
  OvertimeAuthorization a;
  a.id = r[0].as_int64();
  a.employee_id = r[1].as_int64();
  a.period_id = r[2].as_int64();
  a.authorized_minutes = r[3].as_int64();
  a.note = r[4].as_text();
  a.authorized_by_tid = r[5].as_text();
  a.authorized_by_oid = r[6].as_text();
  a.authorized_at = r[7].as_int64();
  a.audit_id = r[8].as_int64();
  return a;
}

Row TimesheetHold::to_row() const {
  return {Value::integer(id),     Value::integer(employee_id),
          Value::integer(period_id), Value::text(reason),
          Value::text(detail),    Value::timestamp(opened_at),
          cleared_at == 0 ? Value::null() : Value::timestamp(cleared_at),
          int_or_null(authorization_id), Value::integer(audit_id)};
}
TimesheetHold TimesheetHold::from_row(const Row& r) {
  TimesheetHold h;
  h.id = r[0].as_int64();
  h.employee_id = r[1].as_int64();
  h.period_id = r[2].as_int64();
  h.reason = r[3].as_text();
  h.detail = r[4].as_text();
  h.opened_at = r[5].as_int64();
  h.cleared_at = int_or_zero(r[6]);
  h.authorization_id = int_or_zero(r[7]);
  h.audit_id = r[8].as_int64();
  return h;
}

Row BreakPremiumRule::to_row() const {
  return {Value::integer(id),           Value::text(break_code),        Value::integer(threshold_minutes), Value::text(comparison),
          Value::text(definition),      Value::text(source),            Value::boolean(confirmed),         Value::integer(effective_from_day),
          int_or_null(effective_to_day), Value::text(set_by),           Value::integer(audit_id)};
}
BreakPremiumRule BreakPremiumRule::from_row(const Row& r) {
  BreakPremiumRule b;
  b.id = r[0].as_int64();
  b.break_code = r[1].as_text();
  b.threshold_minutes = r[2].as_int64();
  b.comparison = r[3].as_text();
  b.definition = r[4].as_text();
  b.source = r[5].as_text();
  b.confirmed = r[6].as_bool();
  b.effective_from_day = r[7].as_int64();
  b.effective_to_day = int_or_zero(r[8]);
  b.set_by = r[9].as_text();
  b.audit_id = r[10].as_int64();
  return b;
}

Row MealBreak::to_row() const {
  return {Value::integer(id),          Value::integer(employee_id), Value::integer(out_entry_id), int_or_null(in_entry_id),
          Value::integer(local_day),   Value::timestamp(started_at), ended_at == 0 ? Value::null() : Value::timestamp(ended_at),
          minutes < 0 ? Value::null() : Value::integer(minutes), Value::boolean(premium), int_or_null(premium_rule_id)};
}
MealBreak MealBreak::from_row(const Row& r) {
  MealBreak m;
  m.id = r[0].as_int64();
  m.employee_id = r[1].as_int64();
  m.out_entry_id = r[2].as_int64();
  m.in_entry_id = int_or_zero(r[3]);
  m.local_day = r[4].as_int64();
  m.started_at = r[5].as_int64();
  m.ended_at = int_or_zero(r[6]);
  m.minutes = r[7].is_null() ? -1 : r[7].as_int64();
  m.premium = r[8].as_bool();
  m.premium_rule_id = int_or_zero(r[9]);
  return m;
}

Row EntryEdit::to_row() const {
  return {Value::integer(id), Value::integer(entry_id), Value::text(edit_class), Value::text(reason_code), Value::text(editor),
          Value::timestamp(edited_at), Value::integer(audit_id)};
}
EntryEdit EntryEdit::from_row(const Row& r) {
  EntryEdit e;
  e.id = r[0].as_int64();
  e.entry_id = r[1].as_int64();
  e.edit_class = r[2].as_text();
  e.reason_code = r[3].as_text();
  e.editor = r[4].as_text();
  e.edited_at = r[5].as_int64();
  e.audit_id = r[6].as_int64();
  return e;
}

// ---- scheduled hours -------------------------------------------------------

Result<std::vector<ScheduleHours>> schedule_hours_of(engine::Reader& r, std::int64_t employee_id) {
  return all<ScheduleHours>(r.scan_all("schedule_hours", "schedule_hours_employee", Bound{{Value::integer(employee_id)}},
                                       Bound{{Value::integer(employee_id)}}));
}

Result<std::optional<ScheduleHours>> schedule_hours_at(engine::Reader& r, std::int64_t employee_id, std::int64_t local_day) {
  auto rows = schedule_hours_of(r, employee_id);
  if (!rows.ok()) return rows.status();
  std::optional<ScheduleHours> found;
  for (const ScheduleHours& s : rows.value()) {
    if (s.effective_from_day <= local_day && (s.effective_to_day == 0 || local_day < s.effective_to_day)) found = s;
  }
  return found;
}

Result<std::int64_t> set_schedule_hours(core::Recorder& rec, std::int64_t employee_id, std::int64_t from_day, std::int64_t to_day,
                                        std::int64_t daily_minutes, std::int64_t weekly_minutes, const std::string& set_by, std::int64_t now_us) {
  (void)now_us;
  engine::Writer& w = rec.writer();
  auto existing = schedule_hours_of(w, employee_id);
  if (!existing.ok()) return existing.status();
  constexpr std::int64_t kForever = std::numeric_limits<std::int64_t>::max();
  for (const ScheduleHours& s : existing.value()) {
    const std::int64_t s_end = s.effective_to_day == 0 ? kForever : s.effective_to_day;
    const std::int64_t n_end = to_day == 0 ? kForever : to_day;
    if (from_day < s_end && n_end > s.effective_from_day) {
      return Status::constraint("scheduled hours overlap another row of this employee (" + format_local_day(s.effective_from_day) + " on)");
    }
  }
  auto id = core::next_id(w, "schedule_hours");
  if (!id.ok()) return id.status();
  ScheduleHours s;
  s.id = id.value();
  s.employee_id = employee_id;
  s.effective_from_day = from_day;
  s.effective_to_day = to_day;
  s.daily_minutes = daily_minutes;
  s.weekly_minutes = weekly_minutes;
  s.set_by = set_by;
  s.audit_id = rec.audit_id();
  if (Status st = rec.insert("schedule_hours", s.to_row()); !st.ok()) return st;
  return s.id;
}

// ---- the hold --------------------------------------------------------------

Result<std::vector<TimesheetHold>> holds_in_period(engine::Reader& r, std::int64_t period_id) {
  return all<TimesheetHold>(r.scan_all("timesheet_holds", "timesheet_holds_period", Bound{{Value::integer(period_id)}}, Bound{{Value::integer(period_id)}}));
}

Result<std::optional<TimesheetHold>> open_hold(engine::Reader& r, std::int64_t employee_id, std::int64_t period_id) {
  auto rows = all<TimesheetHold>(r.scan_all("timesheet_holds", "timesheet_holds_timesheet", Bound{{Value::integer(employee_id), Value::integer(period_id)}},
                                            Bound{{Value::integer(employee_id), Value::integer(period_id)}}));
  if (!rows.ok()) return rows.status();
  std::optional<TimesheetHold> found;
  for (const TimesheetHold& h : rows.value()) {
    if (h.cleared_at == 0) found = h;
  }
  return found;
}

Result<std::set<std::int64_t>> held_employees(engine::Reader& r, std::int64_t period_id) {
  auto rows = holds_in_period(r, period_id);
  if (!rows.ok()) return rows.status();
  std::set<std::int64_t> out;
  for (const TimesheetHold& h : rows.value()) {
    if (h.cleared_at == 0) out.insert(h.employee_id);
  }
  return out;
}

Status evaluate_hold(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t period_id, std::int64_t now_us) {
  engine::Writer& w = rec.writer();
  if (period_id == 0) return Status();
  auto period = period_by_id(w, period_id);
  if (!period.ok()) return period.status();
  if (!period.value().has_value() || period.value()->state != "open") return Status();
  auto totals = totals_of(w, cfg, employee_id, period_id);
  if (!totals.ok()) return totals.status();
  std::vector<std::string> breaches;
  std::string reason;
  for (const auto& [day, us] : totals.value().day_us) {
    auto hours = schedule_hours_at(w, employee_id, day);
    if (!hours.ok()) return hours.status();
    if (hours.value() && us > hours.value()->daily_minutes * kUsPerMinute) {
      if (reason.empty()) reason = "daily_hours";
      breaches.push_back(format_local_day(day) + ": " + hours_minutes(us / kUsPerMinute) + " worked, " + hours_minutes(hours.value()->daily_minutes) +
                         " scheduled a day");
    }
  }
  for (const auto& [week_start, us] : totals.value().week_us) {
    auto hours = schedule_hours_at(w, employee_id, week_start);
    if (!hours.ok()) return hours.status();
    if (hours.value() && us > hours.value()->weekly_minutes * kUsPerMinute) {
      if (reason.empty()) reason = "weekly_hours";
      breaches.push_back("week of " + format_local_day(week_start) + ": " + hours_minutes(us / kUsPerMinute) + " worked, " +
                         hours_minutes(hours.value()->weekly_minutes) + " scheduled a week");
    }
  }
  auto auth = latest_authorization(w, employee_id, period_id);
  if (!auth.ok()) return auth.status();
  const std::int64_t authorized = auth.value() ? auth.value()->authorized_minutes : 0;
  auto excess = overtime_excess_minutes(w, employee_id, totals.value());
  if (!excess.ok()) return excess.status();
  const bool needed = !breaches.empty() && excess.value() > authorized;
  auto open = open_hold(w, employee_id, period_id);
  if (!open.ok()) return open.status();
  std::string detail;
  for (std::size_t i = 0; i < breaches.size(); ++i) detail += (i ? "; " : "") + breaches[i];
  if (needed) {
    if (open.value()) {
      TimesheetHold h = *open.value();
      if (h.detail == detail && h.reason == reason) return Status();
      h.detail = detail;
      h.reason = reason;
      return rec.update("timesheet_holds", h.to_row());
    }
    auto id = core::next_id(w, "timesheet_holds");
    if (!id.ok()) return id.status();
    TimesheetHold h;
    h.id = id.value();
    h.employee_id = employee_id;
    h.period_id = period_id;
    h.reason = reason;
    h.detail = detail;
    h.opened_at = now_us;
    h.audit_id = rec.audit_id();
    return rec.insert("timesheet_holds", h.to_row());
  }
  if (open.value()) {
    TimesheetHold h = *open.value();
    h.cleared_at = now_us;
    h.authorization_id = auth.value() ? auth.value()->id : 0;
    return rec.update("timesheet_holds", h.to_row());
  }
  return Status();
}

Status evaluate_employee_holds(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t now_us) {
  auto periods = rec.writer().scan_all("pay_periods");
  if (!periods.ok()) return periods.status();
  for (const Row& row : periods.value()) {
    const PayPeriod p = PayPeriod::from_row(row);
    if (p.state != "open") continue;
    if (Status s = evaluate_hold(rec, cfg, employee_id, p.id, now_us); !s.ok()) return s;
  }
  return Status();
}

Result<OvertimeAuthorization> authorize_overtime(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t period_id, const std::string& note,
                                                 const std::string& tid, const std::string& oid, std::int64_t now_us) {
  engine::Writer& w = rec.writer();
  auto hold = open_hold(w, employee_id, period_id);
  if (!hold.ok()) return hold.status();
  if (!hold.value()) return Status::not_found("no timesheet is held for this employee in this period");
  auto totals = totals_of(w, cfg, employee_id, period_id);
  if (!totals.ok()) return totals.status();
  auto excess = overtime_excess_minutes(w, employee_id, totals.value());
  if (!excess.ok()) return excess.status();
  auto id = core::next_id(w, "overtime_authorizations");
  if (!id.ok()) return id.status();
  OvertimeAuthorization a;
  a.id = id.value();
  a.employee_id = employee_id;
  a.period_id = period_id;
  a.authorized_minutes = excess.value();
  a.note = note;
  a.authorized_by_tid = tid;
  a.authorized_by_oid = oid;
  a.authorized_at = now_us;
  a.audit_id = rec.audit_id();
  if (Status s = rec.insert("overtime_authorizations", a.to_row()); !s.ok()) return s;
  TimesheetHold h = *hold.value();
  h.cleared_at = now_us;
  h.authorization_id = a.id;
  if (Status s = rec.update("timesheet_holds", h.to_row()); !s.ok()) return s;
  return a;
}

// ---- codes -----------------------------------------------------------------

Status remove_annotations(core::Recorder& rec, std::int64_t shift_id) {
  auto rows = rec.writer().scan_all("shift_annotations", "shift_annotations_shift", Bound{{Value::integer(shift_id)}}, Bound{{Value::integer(shift_id)}});
  if (!rows.ok()) return rows.status();
  for (const Row& r : rows.value()) {
    if (Status s = rec.remove("shift_annotations", {r[0]}); !s.ok()) return s;
  }
  return Status();
}

Status record_annotations(core::Recorder& rec, const Config& cfg, const Shift& shift, const TimeEntry& in, const TimeEntry& out) {
  if (Status s = remove_annotations(rec, shift.id); !s.ok()) return s;
  auto lt_in = parse_local_time(in.local_time);
  auto lt_out = parse_local_time(out.local_time);
  if (!lt_in.ok() || !lt_out.ok()) return Status();
  auto scheds = schedules_of(rec.writer(), in.employee_id);
  if (!scheds.ok()) return scheds.status();
  const Schedule* day = nullptr;
  for (const Schedule& s : scheds.value()) {
    if (s.effective_from_day <= lt_in.value().local_day && (s.effective_to_day == 0 || lt_in.value().local_day < s.effective_to_day) &&
        s.weekday == lt_in.value().weekday) {
      day = &s;
    }
  }
  if (day == nullptr) return Status();
  auto clock = [](int minute_of_day) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", (minute_of_day / 60) % 100, minute_of_day % 60);
    return std::string(buf);
  };
  const std::int64_t tol = cfg.schedule_tolerance_minutes;
  std::vector<std::pair<const char*, std::string>> found;
  if (lt_in.value().minute_of_day > day->start_minute + tol) {
    found.emplace_back("L", "in " + clock(lt_in.value().minute_of_day) + ", scheduled " + clock(day->start_minute));
  }
  if (lt_out.value().local_day == lt_in.value().local_day && lt_out.value().minute_of_day < day->end_minute - tol) {
    found.emplace_back("E", "out " + clock(lt_out.value().minute_of_day) + ", scheduled " + clock(day->end_minute));
  }
  for (const auto& [code, detail] : found) {
    auto id = core::next_id(rec.writer(), "shift_annotations");
    if (!id.ok()) return id.status();
    if (Status s = rec.insert("shift_annotations", {Value::integer(id.value()), Value::integer(shift.id), Value::text(code), Value::text(detail)});
        !s.ok()) {
      return s;
    }
  }
  return Status();
}

Result<bool> break_code_active(engine::Reader& r, const std::string& code) {
  auto row = r.get("break_codes", {Value::text(code)});
  if (!row.ok()) return row.status();
  return row.value().has_value() && (*row.value())[3].as_bool();
}

Status mark_entry_break(core::Recorder& rec, std::int64_t entry_id, const std::string& code) {
  auto active = break_code_active(rec.writer(), code);
  if (!active.ok()) return active.status();
  if (!active.value()) return Status::invalid_argument("unknown or inactive break code '" + code + "'");
  return rec.insert("entry_break_codes", {Value::integer(entry_id), Value::text(code)});
}

Result<std::optional<BreakPremiumRule>> premium_rule_at(engine::Reader& r, const std::string& break_code, std::int64_t local_day) {
  auto rows = all<BreakPremiumRule>(r.scan_all("break_premium_rules", "break_premium_rules_code", Bound{{Value::text(break_code)}}, Bound{{Value::text(break_code)}}));
  if (!rows.ok()) return rows.status();
  std::optional<BreakPremiumRule> found;
  for (const BreakPremiumRule& b : rows.value()) {
    if (!b.confirmed) continue;  // stored, never applied
    if (b.effective_from_day <= local_day && (b.effective_to_day == 0 || local_day < b.effective_to_day)) found = b;  // later effective_from wins
  }
  return found;
}

Result<std::int64_t> add_premium_rule(core::Recorder& rec, const std::string& break_code, std::int64_t threshold_minutes, const std::string& comparison,
                                      const std::string& definition, const std::string& source, bool confirmed, std::int64_t from_day,
                                      std::int64_t to_day, const std::string& set_by, std::int64_t now_us) {
  (void)now_us;
  auto active = break_code_active(rec.writer(), break_code);
  if (!active.ok()) return active.status();
  if (!active.value()) return Status::invalid_argument("unknown or inactive break code '" + break_code + "'");
  auto id = core::next_id(rec.writer(), "break_premium_rules");
  if (!id.ok()) return id.status();
  BreakPremiumRule b;
  b.id = id.value();
  b.break_code = break_code;
  b.threshold_minutes = threshold_minutes;
  b.comparison = comparison;
  b.definition = definition;
  b.source = source;
  b.confirmed = confirmed;
  b.effective_from_day = from_day;
  b.effective_to_day = to_day;
  b.set_by = set_by;
  b.audit_id = rec.audit_id();
  if (Status s = rec.insert("break_premium_rules", b.to_row()); !s.ok()) return s;
  return b.id;
}

Status rebuild_meals(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t from_us) {
  (void)cfg;
  engine::Writer& w = rec.writer();
  const std::int64_t window_start = from_us - 24LL * 3600 * 1'000'000;
  auto entries = entries_of(w, employee_id, window_start, std::numeric_limits<std::int64_t>::max() / 2);
  if (!entries.ok()) return entries.status();
  std::vector<TimeEntry> live;
  for (const TimeEntry& e : entries.value()) {
    if (e.superseded_by == 0) live.push_back(e);
  }
  // The meals these punches began are recomputed from scratch.
  for (const TimeEntry& e : live) {
    if (e.kind != "out") continue;
    auto old = w.scan_all("meal_breaks", "meal_breaks_out", Bound{{Value::integer(e.id)}}, Bound{{Value::integer(e.id)}});
    if (!old.ok()) return old.status();
    for (const Row& r : old.value()) {
      if (Status s = rec.remove("meal_breaks", {r[0]}); !s.ok()) return s;
    }
  }
  for (std::size_t i = 0; i < live.size(); ++i) {
    const TimeEntry& out = live[i];
    if (out.kind != "out") continue;
    auto mark = w.get("entry_break_codes", {Value::integer(out.id)});
    if (!mark.ok()) return mark.status();
    if (!mark.value()) continue;
    const std::string code = (*mark.value())[1].as_text();
    auto lt = parse_local_time(out.local_time);
    if (!lt.ok()) continue;
    MealBreak m;
    m.employee_id = employee_id;
    m.out_entry_id = out.id;
    m.local_day = lt.value().local_day;
    m.started_at = out.device_time;
    if (i + 1 < live.size() && live[i + 1].kind == "in") {
      m.in_entry_id = live[i + 1].id;
      m.ended_at = live[i + 1].device_time;
      m.minutes = (m.ended_at - m.started_at) / kUsPerMinute;
      auto rule = premium_rule_at(w, code, m.local_day);
      if (!rule.ok()) return rule.status();
      if (rule.value()) {
        const bool over = rule.value()->comparison == "at_least" ? m.minutes >= rule.value()->threshold_minutes : m.minutes > rule.value()->threshold_minutes;
        m.premium = over;
        m.premium_rule_id = rule.value()->id;
      }
    }
    auto id = core::next_id(w, "meal_breaks");
    if (!id.ok()) return id.status();
    m.id = id.value();
    if (Status s = rec.insert("meal_breaks", m.to_row()); !s.ok()) return s;
  }
  return Status();
}

Result<std::vector<MealBreak>> meals_in_period(engine::Reader& r, std::int64_t employee_id, std::int64_t period_id) {
  auto period = period_by_id(r, period_id);
  if (!period.ok()) return period.status();
  if (!period.value().has_value()) return std::vector<MealBreak>();
  return all<MealBreak>(r.scan_all("meal_breaks", "meal_breaks_employee", Bound{{Value::integer(employee_id), Value::integer(period.value()->start_day)}},
                                   Bound{{Value::integer(employee_id), Value::integer(period.value()->end_day)}}));
}

// ---- edits -----------------------------------------------------------------

Status record_entry_edit(core::Recorder& rec, std::int64_t entry_id, const std::string& edit_class, const std::string& reason_code,
                         const std::string& editor, std::int64_t now_us) {
  auto id = core::next_id(rec.writer(), "entry_edits");
  if (!id.ok()) return id.status();
  EntryEdit e;
  e.id = id.value();
  e.entry_id = entry_id;
  e.edit_class = edit_class;
  e.reason_code = reason_code;
  e.editor = editor;
  e.edited_at = now_us;
  e.audit_id = rec.audit_id();
  return rec.insert("entry_edits", e.to_row());
}

Result<std::vector<EntryEdit>> all_entry_edits(engine::Reader& r) { return all<EntryEdit>(r.scan_all("entry_edits")); }

}  // namespace archivum::punchline
