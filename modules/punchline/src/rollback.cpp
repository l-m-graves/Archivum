#include "archivum/punchline/rollback.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "archivum/punchline/data.h"

namespace archivum::punchline {
namespace {

std::string iso_utc(std::int64_t us) {
  const std::int64_t secs = us / 1'000'000;
  std::int64_t days = secs / 86400;
  std::int64_t rem = secs % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  // civil_from_days (Howard Hinnant), proleptic Gregorian.
  std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp + (mp < 10 ? 3 : -9);
  const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", static_cast<int>(y % 10000), static_cast<int>(m),
                static_cast<int>(d), static_cast<int>(rem / 3600), static_cast<int>((rem % 3600) / 60), static_cast<int>(rem % 60));
  return buf;
}

std::string uuid_text(const engine::UuidBytes& u) { return engine::Value::uuid(u).to_string(); }

}  // namespace

Result<RollbackExport> rollback_export(engine::Reader& reader, std::int64_t period_id, bool released_only, std::int64_t now_us,
                                       int device_batch_size) {
  RollbackExport out;
  out.batches = nlohmann::json::array();
  auto period = period_by_id(reader, period_id);
  if (!period.ok()) return period.status();
  if (!period.value().has_value()) return Status::not_found("no such pay period");
  auto rows = reader.scan_all("shifts", "shifts_period", engine::Bound{{engine::Value::integer(period_id)}},
                              engine::Bound{{engine::Value::integer(period_id)}});
  if (!rows.ok()) return rows.status();
  std::map<std::int64_t, Employee> employees;
  std::map<std::int64_t, Device> devices;
  std::map<std::string, std::vector<nlohmann::json>> per_device;  // device uuid -> entries
  for (const engine::Row& r : rows.value()) {
    Shift sh = Shift::from_row(r);
    if (sh.out_entry_id == 0) {
      ++out.open_shifts_skipped;
      continue;
    }
    if (released_only && sh.state != "released" && sh.state != "locked") continue;
    if (!employees.count(sh.employee_id)) {
      auto e = employee_by_id(reader, sh.employee_id);
      if (!e.ok()) return e.status();
      if (!e.value().has_value()) return Status::corrupt("shift " + std::to_string(sh.id) + " names a missing employee");
      employees[sh.employee_id] = *e.value();
    }
    if (!devices.count(sh.device_id)) {
      auto d = device_by_id(reader, sh.device_id);
      if (!d.ok()) return d.status();
      if (!d.value().has_value()) return Status::corrupt("shift " + std::to_string(sh.id) + " names a missing device");
      devices[sh.device_id] = *d.value();
    }
    auto in = entry_by_id(reader, sh.in_entry_id);
    if (!in.ok()) return in.status();
    if (!in.value().has_value()) return Status::corrupt("shift " + std::to_string(sh.id) + " names a missing in punch");
    const Employee& emp = employees[sh.employee_id];
    if (emp.company.empty() || emp.cost_center.empty()) ++out.employees_without_routing;
    nlohmann::json e;
    e["uuid"] = uuid_text(in.value()->entry_uuid);
    e["employee_id"] = emp.employee_number;
    e["employee_name"] = emp.display_name;
    e["company"] = emp.company;
    e["cost_center"] = emp.cost_center;
    e["clock_in"] = iso_utc(sh.in_time);
    e["clock_out"] = iso_utc(sh.out_time);
    e["minutes"] = (sh.out_time - sh.in_time) / 60'000'000;
    e["note"] = in.value()->note;
    e["archivum_state"] = sh.state;  // ignored by the old server; for the operator
    per_device[uuid_text(devices[sh.device_id].device_uuid)].push_back(std::move(e));
    ++out.shifts;
  }
  for (auto& [device, entries] : per_device) {
    for (std::size_t i = 0; i < entries.size(); i += static_cast<std::size_t>(device_batch_size)) {
      nlohmann::json batch;
      batch["client_version"] = "archivum-rollback";
      batch["device_id"] = device;
      batch["submitted_at"] = iso_utc(now_us);
      batch["entries"] = nlohmann::json::array();
      for (std::size_t k = i; k < entries.size() && k < i + static_cast<std::size_t>(device_batch_size); ++k) {
        batch["entries"].push_back(entries[k]);
      }
      out.batches.push_back(std::move(batch));
    }
  }
  return out;
}

}  // namespace archivum::punchline
