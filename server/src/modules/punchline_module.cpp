#include <utility>
#include <chrono>
#include "punchline_module.h"

#include <set>

#include "archivum/core/recorder.h"
#include "archivum/punchline/data.h"
#include "archivum/punchline/exceptions.h"
#include "archivum/punchline/localtime.h"
#include "archivum/server/log.h"

namespace archivum::server {
namespace {
using engine::Row;
using engine::Value;

Status positive(const nlohmann::json& j, const char* key, std::int64_t& out) {
  if (!j.contains(key)) return Status();
  if (!j[key].is_number_integer()) return Status::invalid_argument(std::string("modules.punchline.") + key + " must be an integer");
  const std::int64_t v = j[key].get<std::int64_t>();
  if (v <= 0) return Status::invalid_argument(std::string("modules.punchline.") + key + " must be positive");
  out = v;
  return Status();
}
}  // namespace

Status PunchlineModule::configure(const nlohmann::json& section) {
  static const std::set<std::string> allowed = {"long_shift_hours",
                                                "clock_divergence_tolerance_seconds",
                                                "device_attested_threshold",
                                                "device_stale_seconds",
                                                "schedule_tolerance_minutes",
                                                "employee_id_rejection_threshold",
                                                "monitor_interval_seconds",
                                                "max_batch_entries"};
  if (!section.is_object()) return Status::invalid_argument("modules.punchline must be an object");
  for (const auto& [k, v] : section.items()) {
    if (allowed.count(k) == 0) return Status::invalid_argument("unknown key '" + k + "' in modules.punchline");
  }
  punchline::Config c;
  if (Status s = positive(section, "long_shift_hours", c.long_shift_hours); !s.ok()) return s;
  if (Status s = positive(section, "clock_divergence_tolerance_seconds", c.clock_divergence_tolerance_seconds); !s.ok()) return s;
  if (Status s = positive(section, "device_attested_threshold", c.device_attested_threshold); !s.ok()) return s;
  if (Status s = positive(section, "device_stale_seconds", c.device_stale_seconds); !s.ok()) return s;
  if (Status s = positive(section, "schedule_tolerance_minutes", c.schedule_tolerance_minutes); !s.ok()) return s;
  if (Status s = positive(section, "employee_id_rejection_threshold", c.employee_id_rejection_threshold); !s.ok()) return s;
  if (Status s = positive(section, "monitor_interval_seconds", c.monitor_interval_seconds); !s.ok()) return s;
  if (Status s = positive(section, "max_batch_entries", c.max_batch_entries); !s.ok()) return s;
  if (c.max_batch_entries > 10000) return Status::invalid_argument("modules.punchline.max_batch_entries must be at most 10000");
  config_ = c;
  return Status();
}

Result<int> PunchlineModule::monitor_once(App& app) {
  const std::int64_t now = app.now_us();
  auto w = app.store().begin_write();
  if (!w.ok()) return w.status();
  core::Actor sys;
  sys.kind = core::Actor::Kind::System;
  sys.account = "monitor";
  core::Recorder rec(*w.value(), app.policy(), sys, "monitor.tick", now);
  if (!rec.status().ok()) return rec.status();
  int opened = 0;
  // Device freshness: an unrevoked device silent for longer than the
  // threshold. A device never heard from counts from its enrollment.
  {
    auto devices = w.value()->scan_all("devices");
    if (!devices.ok()) return devices.status();
    for (const Row& row : devices.value()) {
      punchline::Device d = punchline::Device::from_row(row);
      if (d.revoked_at != 0) continue;
      const std::int64_t last = d.last_seen_at != 0 ? d.last_seen_at : d.enrolled_at;
      if (now - last < config_.device_stale_seconds * 1'000'000) continue;
      punchline::ExceptionKey key;
      key.kind = "device_stale";
      key.device_id = d.id;
      key.employee_id = d.employee_id;
      auto o = punchline::open_exception(rec, key, "no sync or heartbeat for " + std::to_string((now - last) / 3'600'000'000) + " hours", now);
      if (!o.ok()) return o.status();
      if (o.value().created) {
        ++opened;
        log(LogLevel::Alert, "device.stale", {{"device_id", d.id}, {"employee_id", d.employee_id}, {"last_seen_us", last}});
      }
    }
  }
  // Missed out punches: a shift open longer than the long-shift threshold.
  {
    auto shifts = w.value()->scan_all("shifts");
    if (!shifts.ok()) return shifts.status();
    for (const Row& row : shifts.value()) {
      punchline::Shift sh = punchline::Shift::from_row(row);
      if (sh.out_entry_id != 0 || now - sh.in_time < config_.long_shift_hours * 3600 * 1'000'000) continue;
      punchline::ExceptionKey key;
      key.kind = "unpaired_punch";
      key.employee_id = sh.employee_id;
      key.entry_id = sh.in_entry_id;
      key.device_id = sh.device_id;
      auto o = punchline::open_exception(rec, key, "in punch still open past the long-shift threshold", now);
      if (!o.ok()) return o.status();
      if (o.value().created) ++opened;
    }
  }
  // Escalation after cutoff: an open period past submit_by with entries
  // still recorded, or past approve_by with entries still submitted. An
  // unapproved timesheet at cutoff is defined behaviour: it is queued for
  // payroll (who may approve with a documented override) and alerted.
  {
    auto periods = w.value()->scan_all("pay_periods");
    if (!periods.ok()) return periods.status();
    for (const Row& prow : periods.value()) {
      punchline::PayPeriod p = punchline::PayPeriod::from_row(prow);
      if (p.state != "open") continue;
      const bool past_submit = p.submit_by != 0 && now >= p.submit_by;
      const bool past_approve = p.approve_by != 0 && now >= p.approve_by;
      if (!past_submit && !past_approve) continue;
      std::set<std::pair<std::int64_t, std::string>> behind;  // (employee, state)
      auto entries = w.value()->scan_all("time_entries", "time_entries_period", engine::Bound{{Value::integer(p.id)}},
                                         engine::Bound{{Value::integer(p.id)}});
      if (!entries.ok()) return entries.status();
      for (const Row& erow : entries.value()) {
        punchline::TimeEntry e = punchline::TimeEntry::from_row(erow);
        if (e.superseded_by != 0) continue;
        if ((past_submit && e.state == "recorded") || (past_approve && e.state == "submitted")) behind.insert({e.employee_id, e.state});
      }
      for (const auto& [employee, state] : behind) {
        punchline::ExceptionKey key;
        key.kind = "past_cutoff";
        key.employee_id = employee;
        key.period_id = p.id;
        const std::string detail = state == "recorded" ? "not submitted by the submit cutoff" : "not approved by the approval cutoff";
        auto o = punchline::open_exception(rec, key, detail, now);
        if (!o.ok()) return o.status();
        if (o.value().created) {
          ++opened;
          log(LogLevel::Alert, "period.past_cutoff", {{"period_id", p.id}, {"employee_id", employee}, {"state", state}});
        }
      }
    }
  }
  if (opened == 0) {
    w.value()->rollback();  // nothing happened: no audit row for the tick
    return 0;
  }
  if (Status s = w.value()->commit(); !s.ok()) return s;
  return opened;
}

void PunchlineModule::start(App& app) {
  std::lock_guard<std::mutex> lock(mu_);
  if (thread_.joinable()) return;
  stop_ = false;
  thread_ = std::thread([this, app_ptr = &app] { run(app_ptr); });
}

void PunchlineModule::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PunchlineModule::run(App* app) {
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (cv_.wait_for(lock, std::chrono::seconds(config_.monitor_interval_seconds), [this] { return stop_; })) return;
    }
    auto r = monitor_once(*app);
    if (!r.ok()) log(LogLevel::Error, "monitor.failed", {{"error", r.status().to_string()}});
  }
}

}  // namespace archivum::server
