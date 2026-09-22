#include <string>
#include "archivum/punchline/lifecycle.h"

#include <map>
#include <set>

#include "archivum/core/ids.h"
#include "archivum/punchline/exceptions.h"

namespace archivum::punchline {
namespace {
using engine::Value;

int rank(const std::string& state) {
  if (state == "recorded") return 0;
  if (state == "submitted") return 1;
  if (state == "approved") return 2;
  if (state == "released") return 3;
  if (state == "locked") return 4;
  return -1;
}

std::string predecessor(const std::string& to) {
  if (to == "submitted") return "recorded";
  if (to == "approved") return "submitted";
  if (to == "released") return "approved";
  if (to == "locked") return "released";
  return "";
}

Result<std::int64_t> write_approval(core::Recorder& rec, std::int64_t period_id, std::int64_t employee_id, const std::string& from,
                                    const std::string& to, const std::string& tid, const std::string& oid, const std::string& account,
                                    const std::string& note, std::int64_t now_us) {
  auto id = core::next_id(rec.writer(), "approvals");
  if (!id.ok()) return id.status();
  Approval a;
  a.id = id.value();
  a.period_id = period_id;
  a.employee_id = employee_id;
  a.from_state = from;
  a.to_state = to;
  a.acted_by_tid = tid;
  a.acted_by_oid = oid;
  a.acted_by_account = account;
  a.acted_at = now_us;
  a.note = note;
  a.audit_id = rec.audit_id();
  if (Status s = rec.insert("approvals", a.to_row()); !s.ok()) return s;
  return a.id;
}
}  // namespace

Result<TransitionResult> transition(core::Recorder& rec, const TransitionRequest& req) {
  engine::Writer& w = rec.writer();
  const std::string from = predecessor(req.to_state);
  if (from.empty() || req.to_state == "locked") return Status::invalid_argument("to_state must be submitted, approved or released");
  auto period = period_by_id(w, req.period_id);
  if (!period.ok()) return period.status();
  if (!period.value().has_value()) return Status::not_found("no such pay period");
  if (period.value()->state != "open") return Status::constraint("PL-7: pay period is " + period.value()->state);
  if (Status s = rules::transition_allowed(w, req.employee_id, from, req.to_state, req.who, req.now_us); !s.ok()) return s;
  auto entries = entries_in_period(w, req.period_id, req.employee_id);
  if (!entries.ok()) return entries.status();
  TransitionResult res;
  res.from_state = from;
  int movable = 0;
  for (const TimeEntry& e : entries.value()) {
    if (e.superseded_by != 0) continue;
    if (rank(e.state) < rank(from)) {
      return Status::constraint("PL-7: entry " + std::to_string(e.id) + " is still " + e.state + "; it must be " + from + " first");
    }
    if (e.state == from) ++movable;
  }
  if (movable == 0) return Status::constraint("PL-7: nothing is " + from + " for this employee in this period");
  for (TimeEntry e : entries.value()) {
    if (e.superseded_by != 0 || e.state != from) continue;
    e.state = req.to_state;
    if (req.to_state == "approved") e.approval_audit_id = rec.audit_id();
    if (Status s = rec.update("time_entries", e.to_row()); !s.ok()) return s;
    ++res.entries;
  }
  auto shifts = shifts_in_period(w, req.period_id, req.employee_id);
  if (!shifts.ok()) return shifts.status();
  for (Shift sh : shifts.value()) {
    if (sh.state != from) continue;
    sh.state = req.to_state;
    if (Status s = rec.update("shifts", sh.to_row()); !s.ok()) return s;
    ++res.shifts;
  }
  const std::string note = req.who.override_reason.empty() ? req.note : "override: " + req.who.override_reason;
  auto approval = write_approval(rec, req.period_id, req.employee_id, from, req.to_state, req.acted_by_tid, req.acted_by_oid,
                                 req.acted_by_account, note, req.now_us);
  if (!approval.ok()) return approval.status();
  res.approval_id = approval.value();
  if (req.to_state == "approved") {
    // Device-attested marking is resolved through approval: the named
    // supervisor has reviewed the claims.
    ExceptionKey key;
    key.kind = "device_attested_count";
    key.employee_id = req.employee_id;
    key.period_id = req.period_id;
    auto n = resolve_matching(rec, key, "approval", req.now_us);
    if (!n.ok()) return n.status();
    res.exceptions_resolved += n.value();
    key.kind = "past_cutoff";
    n = resolve_matching(rec, key, "approval", req.now_us);
    if (!n.ok()) return n.status();
    res.exceptions_resolved += n.value();
    // Late punches of this employee in this period were just reviewed too.
    auto late = open_exceptions(w, {req.employee_id}, "late_punch");
    if (!late.ok()) return late.status();
    for (const Exception& x : late.value()) {
      if (x.period_id != req.period_id) continue;
      if (Status s = close_exception(rec, x.id, "resolved", "approval", req.now_us); !s.ok()) return s;
      ++res.exceptions_resolved;
    }
  }
  return res;
}

Result<LockResult> lock_period(core::Recorder& rec, std::int64_t period_id, const rules::Standing& who, const std::string& tid,
                               const std::string& oid, const std::string& account, std::int64_t now_us) {
  engine::Writer& w = rec.writer();
  auto period = period_by_id(w, period_id);
  if (!period.ok()) return period.status();
  if (!period.value().has_value()) return Status::not_found("no such pay period");
  if (period.value()->state != "open") return Status::constraint("PL-7: pay period is already " + period.value()->state);
  if (Status s = rules::transition_allowed(w, 0, "released", "locked", who, now_us); !s.ok()) return s;
  std::map<std::int64_t, int> per_employee;
  auto all = w.scan_all("time_entries", "time_entries_period", engine::Bound{{Value::integer(period_id)}},
                        engine::Bound{{Value::integer(period_id)}});
  if (!all.ok()) return all.status();
  LockResult res;
  for (const engine::Row& row : all.value()) {
    TimeEntry e = TimeEntry::from_row(row);
    if (e.superseded_by != 0) continue;
    if (e.state != "released") {
      return Status::constraint("PL-7: entry " + std::to_string(e.id) + " of employee " + std::to_string(e.employee_id) + " is " +
                                e.state + ", not released");
    }
    e.state = "locked";
    if (Status s = rec.update("time_entries", e.to_row()); !s.ok()) return s;
    ++per_employee[e.employee_id];
    ++res.entries;
  }
  auto shifts = w.scan_all("shifts", "shifts_period", engine::Bound{{Value::integer(period_id)}}, engine::Bound{{Value::integer(period_id)}});
  if (!shifts.ok()) return shifts.status();
  for (const engine::Row& row : shifts.value()) {
    Shift sh = Shift::from_row(row);
    if (sh.state == "released") {
      sh.state = "locked";
      if (Status s = rec.update("shifts", sh.to_row()); !s.ok()) return s;
    }
  }
  for (const auto& [employee, n] : per_employee) {
    (void)n;
    if (auto a = write_approval(rec, period_id, employee, "released", "locked", tid, oid, account, "", now_us); !a.ok()) return a.status();
    ++res.employees;
  }
  PayPeriod p = *period.value();
  p.state = "locked";
  p.release_at = now_us;
  if (Status s = rec.update("pay_periods", p.to_row()); !s.ok()) return s;
  return res;
}

}  // namespace archivum::punchline
