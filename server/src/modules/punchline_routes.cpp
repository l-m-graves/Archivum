// Punchline routes. Stage 5: employees, device enrollment and revocation,
// supervisor assignments, heartbeat. Stage 6: sync with idempotent
// batches, manual entries and corrections, pay periods, the approval
// lifecycle, the exception queue, employee self-service, the supervisor
// queue, the payroll export and period audit report, and the
// segregation-of-duties report.
//
// Every mutating route: parse the body (unknown keys and `employee_id`
// refused), authenticate, check the module rules, mutate through the
// recorder, commit. The employee behind a device request is the
// enrollment record's, never the request's; a request that asserts one
// is refused with 400 and, from a device, logged and counted as a tamper
// signal toward an `employee_id_asserted` exception (Stage 6 ruling).
#include <vector>
#include <utility>
#include <map>
#include <cstdlib>
#include <algorithm>
#include <drogon/drogon.h>

#include <set>

#include "../http/request.h"
#include "archivum/core/authz.h"
#include "archivum/core/credentials.h"
#include "archivum/core/ids.h"
#include "archivum/core/schema.h"
#include "archivum/punchline/data.h"
#include "archivum/punchline/exceptions.h"
#include "archivum/punchline/lifecycle.h"
#include "archivum/punchline/localtime.h"
#include "archivum/punchline/rules.h"
#include "archivum/punchline/schema.h"
#include "archivum/punchline/sync.h"
#include "archivum/server/app.h"
#include "archivum/server/modules.h"
#include "punchline_module.h"

namespace archivum::server {
namespace {

using engine::Row;
using engine::Value;
using Handler = drogon::Task<drogon::HttpResponsePtr>;

drogon::HttpResponsePtr auth_error(const Result<Caller>& who, const std::string& request_id, const char* route) {
  const AuthFailure f = Authenticator::failure_of(who.status());
  log(LogLevel::Warn, "auth.rejected", {{"request_id", request_id}, {"route", route}, {"reason", f.reason}});
  auto resp = error_response(f.http_status, f.http_status == 403 ? "forbidden" : "unauthorized",
                             f.http_status == 403 ? "no standing" : "credential rejected", request_id);
  if (f.http_status == 401) resp->addHeader("WWW-Authenticate", "Bearer");
  return resp;
}

nlohmann::json employee_json(const punchline::Employee& e) {
  nlohmann::json j;
  j["id"] = e.id;
  j["employee_number"] = e.employee_number;
  j["display_name"] = e.display_name;
  j["email"] = e.email;
  j["tid"] = e.tid;
  j["oid"] = e.oid;
  j["active"] = e.active;
  j["pay_group"] = e.pay_group;
  j["site_zone"] = e.site_zone;
  return j;
}

nlohmann::json device_json(const punchline::Device& d) {
  nlohmann::json j;
  j["id"] = d.id;
  j["device_uuid"] = core::uuid_to_string(d.device_uuid);
  j["name"] = d.name;
  j["employee_id"] = d.employee_id;
  j["enrolled_at_us"] = d.enrolled_at;
  j["enrolled_by"] = d.enrolled_by;
  j["revoked"] = d.revoked_at != 0;
  if (d.revoked_at != 0) {
    j["revoked_at_us"] = d.revoked_at;
    j["revoked_reason"] = d.revoked_reason;
  }
  j["last_seen_at_us"] = d.last_seen_at;
  j["last_acked_sequence"] = d.last_acked_sequence;
  j["employee_id_rejections"] = d.employee_id_rejections;
  return j;
}

nlohmann::json entry_json(const punchline::TimeEntry& e) {
  nlohmann::json j;
  j["id"] = e.id;
  j["entry_uuid"] = core::uuid_to_string(e.entry_uuid);
  j["employee_id"] = e.employee_id;
  j["device_id"] = e.device_id;
  if (e.journal_id) j["journal_id"] = core::uuid_to_string(*e.journal_id);
  j["journal_sequence"] = e.journal_sequence;
  j["kind"] = e.kind;
  j["device_time_us"] = e.device_time;
  j["local_time"] = e.local_time;
  j["site_zone"] = e.site_zone;
  j["tzdb_version"] = e.tzdb_version;
  j["receipt_time_us"] = e.receipt_time;
  j["attestation"] = e.attestation;
  if (e.clock_divergence_us) j["clock_divergence_us"] = *e.clock_divergence_us;
  if (e.period_id != 0) j["period_id"] = e.period_id;
  j["state"] = e.state;
  if (!e.pay_code.empty()) j["pay_code"] = e.pay_code;
  if (e.approval_audit_id != 0) j["approval_audit_id"] = e.approval_audit_id;
  if (e.correction_of != 0) {
    j["correction_of"] = e.correction_of;
    j["correction_reason"] = e.correction_reason;
  }
  if (e.superseded_by != 0) j["superseded_by"] = e.superseded_by;
  if (!e.note.empty()) j["note"] = e.note;
  return j;
}

nlohmann::json shift_json(const punchline::Shift& s) {
  nlohmann::json j;
  j["id"] = s.id;
  j["employee_id"] = s.employee_id;
  j["device_id"] = s.device_id;
  j["in_entry_id"] = s.in_entry_id;
  if (s.out_entry_id != 0) j["out_entry_id"] = s.out_entry_id;
  j["local_day"] = punchline::format_local_day(s.local_day);
  if (s.period_id != 0) j["period_id"] = s.period_id;
  j["in_time_us"] = s.in_time;
  if (s.out_time != 0) j["out_time_us"] = s.out_time;
  if (s.duration_us) j["duration_us"] = *s.duration_us;
  j["open"] = s.out_entry_id == 0;
  if (!s.pay_code.empty()) j["pay_code"] = s.pay_code;
  j["state"] = s.state;
  return j;
}

nlohmann::json exception_json(const punchline::Exception& x) {
  nlohmann::json j;
  j["id"] = x.id;
  j["kind"] = x.kind;
  j["state"] = x.state;
  if (x.employee_id != 0) j["employee_id"] = x.employee_id;
  if (x.device_id != 0) j["device_id"] = x.device_id;
  if (x.entry_id != 0) j["entry_id"] = x.entry_id;
  if (x.period_id != 0) j["period_id"] = x.period_id;
  j["detail"] = x.detail;
  j["opened_at_us"] = x.opened_at;
  if (x.resolved_at != 0) {
    j["resolved_at_us"] = x.resolved_at;
    j["resolved_by"] = x.resolved_by;
    j["resolution_audit_id"] = x.resolution_audit_id;
  }
  return j;
}

nlohmann::json period_json(const punchline::PayPeriod& p) {
  nlohmann::json j;
  j["id"] = p.id;
  j["start_day"] = punchline::format_local_day(p.start_day);
  j["end_day"] = punchline::format_local_day(p.end_day);
  j["site_zone"] = p.site_zone;
  j["tzdb_version"] = p.tzdb_version;
  j["state"] = p.state;
  if (p.submit_by != 0) j["submit_by_us"] = p.submit_by;
  if (p.approve_by != 0) j["approve_by_us"] = p.approve_by;
  if (p.release_at != 0) j["locked_at_us"] = p.release_at;
  return j;
}

punchline::rules::Standing standing_of(const Caller& c, std::string override_reason = "") {
  punchline::rules::Standing s;
  s.roles = c.roles;
  s.employee_id = c.employee_id;
  s.override_reason = std::move(override_reason);
  if (c.kind == Caller::Kind::Account && c.account_kind == "break_glass") s.roles.insert("admin");
  return s;
}

Result<std::int64_t> path_id(const std::string& text) {
  char* end = nullptr;
  const long long id = std::strtoll(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || id <= 0) return Status::invalid_argument("bad id in path");
  return static_cast<std::int64_t>(id);
}

// A body parse failure. When it is the employee_id contract, the request
// is identified anyway so the signal is attributable: from a device it is
// logged at warning with the device and the offending key, counted on the
// device row, and counted toward an `employee_id_asserted` exception once
// the threshold is reached. Never audited as a business action (there is
// none), never silently ignored.
drogon::Task<drogon::HttpResponsePtr> body_rejected(App* self, std::shared_ptr<PunchlineModule> mod, drogon::HttpRequestPtr req,
                                                    std::string request_id, Status status, const char* route) {
  if (!is_employee_id_rejection(status)) co_return status_response(status, request_id);
  const std::string key = rejected_key_path(status);
  auto who = co_await self->authenticator().authenticate(req, request_id);
  if (!who.ok()) {
    log(LogLevel::Warn, "request.employee_id_asserted",
        {{"request_id", request_id}, {"route", route}, {"key", key}, {"caller", "unauthenticated"}});
    co_return status_response(status, request_id);
  }
  const Caller& c = who.value();
  if (c.kind != Caller::Kind::Device) {
    log(LogLevel::Warn, "request.employee_id_asserted", {{"request_id", request_id}, {"route", route}, {"key", key}, {"caller", c.describe()}});
    co_return status_response(status, request_id);
  }
  std::int64_t count = 0;
  bool opened = false;
  {
    auto w = self->store().begin_write();
    if (w.ok()) {
      auto dev = punchline::device_by_id(*w.value(), c.device_id);
      if (dev.ok() && dev.value().has_value()) {
        punchline::Device d = *dev.value();
        count = ++d.employee_id_rejections;
        // The counter is operational state; the exception it leads to is the audited event.
        Status s = w.value()->update("devices", d.to_row());
        if (s.ok() && count >= mod->config().employee_id_rejection_threshold) {
          core::Recorder rec(*w.value(), self->policy(), c.actor(), "device.tamper_signal", self->now_us(),
                             "employee id asserted " + std::to_string(count) + " times");
          if (rec.status().ok()) {
            punchline::ExceptionKey k;
            k.kind = "employee_id_asserted";
            k.device_id = d.id;
            k.employee_id = d.employee_id;
            auto o = punchline::open_exception(rec, k, "device asserted an employee id in " + std::to_string(count) + " requests", self->now_us());
            if (o.ok()) {
              opened = o.value().created;
              (void)rec.set_target("exceptions", std::to_string(o.value().id));
            }
          }
        }
        if (s.ok()) (void)w.value()->commit();
      }
    }
  }
  log(LogLevel::Warn, "request.employee_id_asserted",
      {{"request_id", request_id}, {"route", route}, {"key", key}, {"device", core::uuid_to_string(c.device_uuid)}, {"count", count}});
  if (opened) {
    log(LogLevel::Alert, "device.tamper_signal", {{"request_id", request_id}, {"device", core::uuid_to_string(c.device_uuid)}, {"count", count}});
  }
  co_return status_response(status, request_id);
}

// The employees a caller may act for: their reports (supervisor, at now),
// everyone (payroll, admin), or themself.
Result<std::vector<std::int64_t>> scope_of(engine::Reader& rd, const Caller& c, std::int64_t now_us, bool& all) {
  all = c.has_role("payroll") || c.has_role("admin") || (c.kind == Caller::Kind::Account && c.account_kind == "break_glass");
  if (all) return std::vector<std::int64_t>();
  std::vector<std::int64_t> out;
  if (c.has_role("supervisor") && c.employee_id) {
    auto reports = punchline::reports_of(rd, *c.employee_id, now_us);
    if (!reports.ok()) return reports.status();
    out = reports.value();
  }
  if (c.employee_id) out.push_back(*c.employee_id);
  return out;
}

bool in_scope(const std::vector<std::int64_t>& scope, bool all, std::int64_t employee_id) {
  return all || std::find(scope.begin(), scope.end(), employee_id) != scope.end();
}

std::string csv_field(const std::string& s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

}  // namespace

void PunchlineModule::register_routes(App& app_ref) {
  App* self = &app_ref;
  auto mod = shared_from_this();
  auto& app = drogon::app();

  // ---- Stage 5 routes ----------------------------------------------------

  app.registerHandler(
      "/api/v1/admin/employees",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        if (req->method() == drogon::Get) {
          auto who = co_await self->authenticator().authenticate(req, request_id);
          if (!who.ok()) co_return auth_error(who, request_id, "employees.list");
          if (!who.value().has_role("admin") && !who.value().has_role("payroll")) {
            co_return error_response(403, "forbidden", "admin or payroll role required", request_id);
          }
          auto rd = self->store().begin_read();
          if (!rd.ok()) co_return status_response(rd.status(), request_id);
          auto rows = rd.value()->scan_all("employees");
          if (!rows.ok()) co_return status_response(rows.status(), request_id);
          nlohmann::json list = nlohmann::json::array();
          for (const auto& r : rows.value()) list.push_back(employee_json(punchline::Employee::from_row(r)));
          co_return ok_response({{"employees", list}}, request_id);
        }
        auto body = parse_body(req, {"employee_number", "display_name", "email", "tid", "oid", "site_zone", "pay_group"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "employees.create");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "employees.create");
        if (!who.value().has_role("admin")) co_return error_response(403, "forbidden", "admin role required", request_id);
        punchline::Employee e;
        auto number = body_string(body.value(), "employee_number");
        auto name = body_string(body.value(), "display_name");
        auto zone = body_string(body.value(), "site_zone");
        auto email = body_string(body.value(), "email", false);
        auto tid = body_string(body.value(), "tid", false);
        auto oid = body_string(body.value(), "oid", false);
        auto pay_group = body_string(body.value(), "pay_group", false);
        for (const auto* r : {&number, &name, &zone, &email, &tid, &oid, &pay_group}) {
          if (!r->ok()) co_return status_response(r->status(), request_id);
        }
        if (tid.value().empty() != oid.value().empty()) co_return error_response(400, "invalid", "tid and oid go together", request_id);
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto id = core::next_id(*w.value(), "employees");
        if (!id.ok()) co_return status_response(id.status(), request_id);
        e.id = id.value();
        e.employee_number = number.value();
        e.display_name = name.value();
        e.site_zone = zone.value();
        e.email = email.value();
        e.tid = tid.value();
        e.oid = oid.value();
        e.pay_group = pay_group.value();
        e.created_at = e.updated_at = self->now_us();
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "employee.create", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        if (Status s = rec.insert("employees", e.to_row()); !s.ok()) co_return status_response(s, request_id);
        if (Status s = rec.set_target("employees", std::to_string(e.id)); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        log(LogLevel::Info, "employee.created", {{"request_id", request_id}, {"by", who.value().describe()}, {"employee_id", e.id}});
        co_return ok_response(employee_json(e), request_id, 201);
      },
      {drogon::Get, drogon::Post});

  app.registerHandler(
      "/api/v1/admin/devices",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        if (req->method() == drogon::Get) {
          auto who = co_await self->authenticator().authenticate(req, request_id);
          if (!who.ok()) co_return auth_error(who, request_id, "devices.list");
          if (!who.value().has_role("admin") && !who.value().has_role("device_admin")) {
            co_return error_response(403, "forbidden", "admin role required", request_id);
          }
          auto rd = self->store().begin_read();
          if (!rd.ok()) co_return status_response(rd.status(), request_id);
          auto rows = rd.value()->scan_all("devices");
          if (!rows.ok()) co_return status_response(rows.status(), request_id);
          nlohmann::json list = nlohmann::json::array();
          for (const auto& r : rows.value()) list.push_back(device_json(punchline::Device::from_row(r)));
          co_return ok_response({{"devices", list}}, request_id);
        }
        auto body = parse_body(req, {"employee_number", "name"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "devices.enroll");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "devices.enroll");
        if (!who.value().has_role("admin") && !who.value().has_role("device_admin")) {
          co_return error_response(403, "forbidden", "admin role required", request_id);
        }
        auto number = body_string(body.value(), "employee_number");
        auto name = body_string(body.value(), "name");
        if (!number.ok()) co_return status_response(number.status(), request_id);
        if (!name.ok()) co_return status_response(name.status(), request_id);
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto emp = punchline::employee_by_number(*w.value(), number.value());
        if (!emp.ok()) co_return status_response(emp.status(), request_id);
        if (!emp.value().has_value()) co_return error_response(404, "not_found", "no such employee", request_id);
        if (!emp.value()->active) co_return error_response(409, "constraint", "employee inactive", request_id);
        if (Status s = punchline::rules::one_active_device(*w.value(), emp.value()->id); !s.ok()) co_return status_response(s, request_id);
        auto id = core::next_id(*w.value(), "devices");
        if (!id.ok()) co_return status_response(id.status(), request_id);
        const std::string secret = core::random_secret(32);
        auto verifier = core::make_verifier(secret);
        if (!verifier.ok()) co_return status_response(verifier.status(), request_id);
        punchline::Device d;
        d.id = id.value();
        d.device_uuid = core::random_uuid();
        d.name = name.value();
        d.employee_id = emp.value()->id;
        d.credential_hash = verifier.value();
        d.enrolled_at = self->now_us();
        d.enrolled_by = who.value().describe();
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "device.enroll", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        if (Status s = rec.insert("devices", d.to_row()); !s.ok()) co_return status_response(s, request_id);
        if (Status s = rec.set_target("devices", std::to_string(d.id)); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        log(LogLevel::Info, "device.enrolled",
            {{"request_id", request_id}, {"by", who.value().describe()}, {"device_id", d.id}, {"employee_id", d.employee_id}});
        nlohmann::json out = device_json(d);
        out["credential"] = core::uuid_to_string(d.device_uuid) + ":" + secret;  // shown once
        co_return ok_response(out, request_id, 201);
      },
      {drogon::Get, drogon::Post});

  app.registerHandler(
      "/api/v1/admin/devices/{uuid}/revoke",
      [self, mod](drogon::HttpRequestPtr req, std::string uuid_text) -> Handler {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"reason"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "devices.revoke");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "devices.revoke");
        if (!who.value().has_role("admin") && !who.value().has_role("device_admin")) {
          co_return error_response(403, "forbidden", "admin role required", request_id);
        }
        auto reason = body_string(body.value(), "reason");
        if (!reason.ok()) co_return status_response(reason.status(), request_id);
        auto uuid = core::uuid_from_string(uuid_text);
        if (!uuid.ok()) co_return error_response(400, "invalid", "bad device id", request_id);
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto dev = punchline::device_by_uuid(*w.value(), uuid.value());
        if (!dev.ok()) co_return status_response(dev.status(), request_id);
        if (!dev.value().has_value()) co_return error_response(404, "not_found", "no such device", request_id);
        punchline::Device d = *dev.value();
        if (d.revoked_at != 0) co_return error_response(409, "constraint", "already revoked", request_id);
        d.revoked_at = self->now_us();
        d.revoked_reason = reason.value();
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "device.revoke", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        if (Status s = rec.update("devices", d.to_row()); !s.ok()) co_return status_response(s, request_id);
        if (Status s = rec.set_target("devices", std::to_string(d.id)); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        log(LogLevel::Info, "device.revoked", {{"request_id", request_id}, {"by", who.value().describe()}, {"device_id", d.id}});
        co_return ok_response(device_json(d), request_id);
      },
      {drogon::Post});

  app.registerHandler(
      "/api/v1/admin/supervisors",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        if (req->method() == drogon::Get) {
          auto who = co_await self->authenticator().authenticate(req, request_id);
          if (!who.ok()) co_return auth_error(who, request_id, "supervisors.list");
          if (!who.value().has_role("admin") && !who.value().has_role("payroll")) {
            co_return error_response(403, "forbidden", "admin or payroll role required", request_id);
          }
          auto rd = self->store().begin_read();
          if (!rd.ok()) co_return status_response(rd.status(), request_id);
          auto rows = rd.value()->scan_all("supervisor_assignments");
          if (!rows.ok()) co_return status_response(rows.status(), request_id);
          nlohmann::json list = nlohmann::json::array();
          for (const auto& r : rows.value()) {
            punchline::SupervisorAssignment a = punchline::SupervisorAssignment::from_row(r);
            nlohmann::json j;
            j["id"] = a.id;
            j["employee_id"] = a.employee_id;
            j["supervisor_employee_id"] = a.supervisor_employee_id;
            j["effective_from_us"] = a.effective_from;
            if (a.effective_to != 0) j["effective_to_us"] = a.effective_to;
            j["assigned_by"] = a.assigned_by;
            j["audit_id"] = a.audit_id;
            list.push_back(j);
          }
          co_return ok_response({{"assignments", list}}, request_id);
        }
        auto body = parse_body(req, {"employee_number", "supervisor_employee_number", "effective_from_us", "effective_to_us"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "supervisors.assign");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "supervisors.assign");
        if (!who.value().has_role("admin")) co_return error_response(403, "forbidden", "admin role required", request_id);
        auto number = body_string(body.value(), "employee_number");
        auto sup_number = body_string(body.value(), "supervisor_employee_number");
        auto from = body_int(body.value(), "effective_from_us");
        auto to = body_int(body.value(), "effective_to_us", false);
        for (const Status& s : {number.status(), sup_number.status(), from.status(), to.status()}) {
          if (!s.ok()) co_return status_response(s, request_id);
        }
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto emp = punchline::employee_by_number(*w.value(), number.value());
        auto sup = punchline::employee_by_number(*w.value(), sup_number.value());
        if (!emp.ok()) co_return status_response(emp.status(), request_id);
        if (!sup.ok()) co_return status_response(sup.status(), request_id);
        if (!emp.value().has_value() || !sup.value().has_value()) co_return error_response(404, "not_found", "no such employee", request_id);
        if (Status s = punchline::rules::supervisor_assignment_valid(*w.value(), emp.value()->id, sup.value()->id, from.value(), to.value());
            !s.ok()) {
          co_return status_response(s, request_id);
        }
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "supervisor.assign", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        auto id = core::next_id(*w.value(), "supervisor_assignments");
        if (!id.ok()) co_return status_response(id.status(), request_id);
        punchline::SupervisorAssignment a;
        a.id = id.value();
        a.employee_id = emp.value()->id;
        a.supervisor_employee_id = sup.value()->id;
        a.effective_from = from.value();
        a.effective_to = to.value();
        a.assigned_by = who.value().describe();
        a.audit_id = rec.audit_id();
        if (Status s = rec.insert("supervisor_assignments", a.to_row()); !s.ok()) co_return status_response(s, request_id);
        if (Status s = rec.set_target("supervisor_assignments", std::to_string(id.value())); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        co_return ok_response({{"id", id.value()}}, request_id, 201);
      },
      {drogon::Get, drogon::Post});

  // Weekly schedules, HR-sourced (admin or payroll). No row means "no
  // schedule on file", which the pairing check flags rather than checks.
  app.registerHandler(
      "/api/v1/admin/schedules",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"employee_number", "weekday", "start_minute", "end_minute", "effective_from_day", "effective_to_day"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "schedules.create");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "schedules.create");
        if (!who.value().has_role("admin") && !who.value().has_role("payroll")) {
          co_return error_response(403, "forbidden", "admin or payroll role required", request_id);
        }
        auto number = body_string(body.value(), "employee_number");
        auto weekday = body_int(body.value(), "weekday");
        auto start = body_int(body.value(), "start_minute");
        auto end = body_int(body.value(), "end_minute");
        auto from = body_string(body.value(), "effective_from_day");
        auto to = body_string(body.value(), "effective_to_day", false);
        for (const Status& s : {number.status(), weekday.status(), start.status(), end.status(), from.status(), to.status()}) {
          if (!s.ok()) co_return status_response(s, request_id);
        }
        auto fd = punchline::parse_local_time(from.value() + "T00:00:00");
        if (!fd.ok()) co_return error_response(400, "invalid", "effective_from_day must be YYYY-MM-DD", request_id);
        std::int64_t to_day = 0;
        if (!to.value().empty()) {
          auto td = punchline::parse_local_time(to.value() + "T00:00:00");
          if (!td.ok()) co_return error_response(400, "invalid", "effective_to_day must be YYYY-MM-DD", request_id);
          to_day = td.value().local_day;
        }
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto emp = punchline::employee_by_number(*w.value(), number.value());
        if (!emp.ok()) co_return status_response(emp.status(), request_id);
        if (!emp.value().has_value()) co_return error_response(404, "not_found", "no such employee", request_id);
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "schedule.create", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        auto id = core::next_id(*w.value(), "schedules");
        if (!id.ok()) co_return status_response(id.status(), request_id);
        punchline::Schedule sch;
        sch.id = id.value();
        sch.employee_id = emp.value()->id;
        sch.weekday = static_cast<int>(weekday.value());
        sch.start_minute = static_cast<int>(start.value());
        sch.end_minute = static_cast<int>(end.value());
        sch.effective_from_day = fd.value().local_day;
        sch.effective_to_day = to_day;
        if (Status s = rec.insert("schedules", sch.to_row()); !s.ok()) co_return status_response(s, request_id);
        if (Status s = rec.set_target("schedules", std::to_string(sch.id)); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        co_return ok_response({{"id", sch.id}}, request_id, 201);
      },
      {drogon::Post});

  app.registerHandler(
      "/api/v1/device/heartbeat",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"client_time_us", "last_journal_sequence"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "device.heartbeat");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) {
          auto resp = auth_error(who, request_id, "device.heartbeat");
          resp->addHeader("WWW-Authenticate", "Device");
          co_return resp;
        }
        if (who.value().kind != Caller::Kind::Device) co_return error_response(403, "forbidden", "device credential required", request_id);
        auto client_time = body_int(body.value(), "client_time_us", false);
        if (!client_time.ok()) co_return status_response(client_time.status(), request_id);
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto dev = punchline::device_by_id(*w.value(), who.value().device_id);
        if (!dev.ok() || !dev.value().has_value()) co_return error_response(404, "not_found", "device vanished", request_id);
        punchline::Device d = *dev.value();
        d.last_seen_at = self->now_us();
        auto emp = punchline::employee_by_id(*w.value(), d.employee_id);
        if (!emp.ok() || !emp.value().has_value()) co_return error_response(404, "not_found", "employee vanished", request_id);
        // last_seen is operational state, not an audited change: plain
        // update. Unless the device was flagged stale: resolving that is a
        // queue change and goes through the recorder.
        if (Status s = w.value()->update("devices", d.to_row()); !s.ok()) co_return status_response(s, request_id);
        {
          punchline::ExceptionKey stale;
          stale.kind = "device_stale";
          stale.device_id = d.id;
          stale.employee_id = d.employee_id;
          auto open = punchline::open_exceptions(*w.value(), {d.employee_id}, "device_stale");
          if (!open.ok()) co_return status_response(open.status(), request_id);
          if (!open.value().empty()) {
            core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "device.heartbeat", self->now_us());
            if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
            if (auto r = punchline::resolve_matching(rec, stale, "heartbeat", self->now_us()); !r.ok()) co_return status_response(r.status(), request_id);
          }
        }
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        nlohmann::json out;
        out["employee"]["display_name"] = emp.value()->display_name;
        out["employee"]["employee_number"] = emp.value()->employee_number;
        out["employee"]["site_zone"] = emp.value()->site_zone;
        out["server_time_us"] = self->now_us();
        out["last_acked_sequence"] = d.last_acked_sequence;
        if (client_time.value() != 0) out["clock_divergence_us"] = self->now_us() - client_time.value();
        co_return ok_response(out, request_id);
      },
      {drogon::Post});

  // ---- Stage 6: sync ------------------------------------------------------

  // The device's batch: idempotent by batch uuid and entry uuid, one write
  // transaction under one audit row, per-entry outcomes so one bad entry
  // never sinks the batch (the client can only retry a batch as a unit).
  app.registerHandler(
      "/api/v1/device/sync",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"batch_uuid", "journal_id", "client_time_us", "entries", "reports"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "device.sync");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) {
          auto resp = auth_error(who, request_id, "device.sync");
          resp->addHeader("WWW-Authenticate", "Device");
          co_return resp;
        }
        if (who.value().kind != Caller::Kind::Device) co_return error_response(403, "forbidden", "device credential required", request_id);
        const nlohmann::json& b = body.value();
        punchline::IncomingBatch batch;
        auto batch_uuid = body_string(b, "batch_uuid");
        auto journal_id = body_string(b, "journal_id");
        auto client_time = body_int(b, "client_time_us", false);
        for (const Status& s : {batch_uuid.status(), journal_id.status(), client_time.status()}) {
          if (!s.ok()) co_return status_response(s, request_id);
        }
        auto bu = core::uuid_from_string(batch_uuid.value());
        auto ju = core::uuid_from_string(journal_id.value());
        if (!bu.ok()) co_return error_response(400, "invalid", "batch_uuid is not a uuid", request_id);
        if (!ju.ok()) co_return error_response(400, "invalid", "journal_id is not a uuid", request_id);
        batch.batch_uuid = bu.value();
        batch.journal_id = ju.value();
        batch.client_time_us = client_time.value();
        if (!b.contains("entries") || !b["entries"].is_array()) co_return error_response(400, "invalid", "entries must be an array", request_id);
        if (static_cast<std::int64_t>(b["entries"].size()) > mod->config().max_batch_entries) {
          co_return error_response(413, "too_large", "batch exceeds " + std::to_string(mod->config().max_batch_entries) + " entries", request_id);
        }
        // Each entry is validated on its own inside apply_batch so the
        // response can accept the good ones and name the bad ones; here
        // only the shape that cannot be attributed to an entry is refused.
        std::vector<std::pair<std::string, std::string>> shape_rejections;  // (uuid as sent, reason)
        for (const nlohmann::json& je : b["entries"]) {
          punchline::IncomingEntry e;
          if (!je.is_object()) co_return error_response(400, "invalid", "each entry must be an object", request_id);
          static const std::set<std::string> allowed = {"entry_uuid", "journal_sequence", "kind",     "device_time_us", "local_time",
                                                        "site_zone",  "tzdb_version",     "pay_code", "note"};
          std::string bad_key;
          for (const auto& [k, v] : je.items()) {
            if (allowed.count(k) == 0) bad_key = k;
          }
          const std::string raw_uuid = je.contains("entry_uuid") && je["entry_uuid"].is_string() ? je["entry_uuid"].get<std::string>() : "";
          auto uuid = core::uuid_from_string(raw_uuid);
          if (!uuid.ok()) {
            shape_rejections.emplace_back(raw_uuid.substr(0, 64), raw_uuid.empty() ? "entry_uuid is required" : "entry_uuid is not a uuid");
            continue;
          }
          e.entry_uuid = uuid.value();
          auto seq = body_int(je, "journal_sequence");
          auto kind = body_string(je, "kind", false);
          auto at = body_int(je, "device_time_us", false);
          auto local = body_string(je, "local_time", false);
          auto zone = body_string(je, "site_zone", false);
          auto tz = body_string(je, "tzdb_version", false);
          auto code = body_string(je, "pay_code", false);
          auto note = body_string(je, "note", false);
          std::string problem = bad_key.empty() ? "" : "unknown key '" + bad_key + "'";
          for (const Status* s : {&seq.status(), &kind.status(), &at.status(), &local.status(), &zone.status(), &tz.status(), &code.status(),
                                  &note.status()}) {
            if (!s->ok() && problem.empty()) problem = s->message();
          }
          if (!problem.empty()) {
            shape_rejections.emplace_back(raw_uuid, problem);
            continue;
          }
          e.journal_sequence = seq.value();
          e.kind = kind.value();
          e.device_time_us = at.value();
          e.local_time = local.value();
          e.site_zone = zone.value();
          e.tzdb_version = tz.value();
          e.pay_code = code.value();
          e.note = note.value();
          batch.entries.push_back(std::move(e));
        }
        if (b.contains("reports")) {
          if (!b["reports"].is_array()) co_return error_response(400, "invalid", "reports must be an array", request_id);
          for (const nlohmann::json& jr : b["reports"]) {
            if (!jr.is_object()) co_return error_response(400, "invalid", "each report must be an object", request_id);
            for (const auto& [k, v] : jr.items()) {
              if (k != "kind" && k != "detail") co_return error_response(400, "invalid", "unknown key '" + k + "' in report", request_id);
            }
            auto kind = body_string(jr, "kind");
            auto detail = body_string(jr, "detail", false);
            if (!kind.ok()) co_return status_response(kind.status(), request_id);
            if (!detail.ok()) co_return status_response(detail.status(), request_id);
            batch.reports.push_back({kind.value(), detail.value()});
          }
        }
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto dev = punchline::device_by_id(*w.value(), who.value().device_id);
        if (!dev.ok() || !dev.value().has_value()) co_return error_response(404, "not_found", "device vanished", request_id);
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "sync.batch", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        auto outcome = punchline::apply_batch(rec, mod->config(), *dev.value(), batch, self->now_us());
        if (!outcome.ok()) co_return status_response(outcome.status(), request_id);
        if (Status s = rec.set_target("sync_batches", batch_uuid.value()); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        const punchline::SyncOutcome& o = outcome.value();
        nlohmann::json out;
        out["accepted"] = nlohmann::json::array();
        out["rejected"] = nlohmann::json::array();
        for (const punchline::EntryOutcome& oc : o.outcomes) {
          if (oc.accepted) {
            out["accepted"].push_back(core::uuid_to_string(oc.entry_uuid));
          } else {
            out["rejected"].push_back({{"uuid", core::uuid_to_string(oc.entry_uuid)}, {"reason", oc.reason}});
          }
        }
        for (const auto& [raw, reason] : shape_rejections) out["rejected"].push_back({{"uuid", raw}, {"reason", reason}});
        out["acked_sequences"] = o.acked_sequences;
        out["last_acked_sequence"] = o.last_acked_sequence;
        out["replayed"] = o.replayed;
        if (o.clock_divergence_us) out["clock_divergence_us"] = *o.clock_divergence_us;
        out["exceptions_opened"] = o.exceptions_opened.size();
        out["server_time_us"] = self->now_us();
        log(LogLevel::Info, "sync.batch",
            {{"request_id", request_id}, {"device", who.value().describe()}, {"accepted", o.accepted},
             {"rejected", o.rejected + static_cast<int>(shape_rejections.size())}, {"replayed", o.replayed},
             {"exceptions_opened", o.exceptions_opened.size()}});
        co_return ok_response(out, request_id);
      },
      {drogon::Post});

  // ---- Stage 6: pay periods -----------------------------------------------

  app.registerHandler(
      "/api/v1/admin/periods",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        if (req->method() == drogon::Get) {
          auto who = co_await self->authenticator().authenticate(req, request_id);
          if (!who.ok()) co_return auth_error(who, request_id, "periods.list");
          auto rd = self->store().begin_read();
          if (!rd.ok()) co_return status_response(rd.status(), request_id);
          auto rows = rd.value()->scan_all("pay_periods", "pay_periods_start");
          if (!rows.ok()) co_return status_response(rows.status(), request_id);
          nlohmann::json list = nlohmann::json::array();
          for (const auto& r : rows.value()) list.push_back(period_json(punchline::PayPeriod::from_row(r)));
          co_return ok_response({{"periods", list}}, request_id);
        }
        auto body = parse_body(req, {"start_day", "end_day", "site_zone", "tzdb_version", "submit_by_us", "approve_by_us"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "periods.create");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "periods.create");
        if (!who.value().has_role("admin") && !who.value().has_role("payroll")) {
          co_return error_response(403, "forbidden", "admin or payroll role required", request_id);
        }
        auto start = body_string(body.value(), "start_day");
        auto end = body_string(body.value(), "end_day");
        auto zone = body_string(body.value(), "site_zone");
        auto tz = body_string(body.value(), "tzdb_version");
        auto submit_by = body_int(body.value(), "submit_by_us", false);
        auto approve_by = body_int(body.value(), "approve_by_us", false);
        for (const Status& s : {start.status(), end.status(), zone.status(), tz.status(), submit_by.status(), approve_by.status()}) {
          if (!s.ok()) co_return status_response(s, request_id);
        }
        auto sd = punchline::parse_local_time(start.value() + "T00:00:00");
        auto ed = punchline::parse_local_time(end.value() + "T00:00:00");
        if (!sd.ok() || !ed.ok()) co_return error_response(400, "invalid", "start_day and end_day must be YYYY-MM-DD", request_id);
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        // No overlap with an existing period.
        auto existing = w.value()->scan_all("pay_periods");
        if (!existing.ok()) co_return status_response(existing.status(), request_id);
        for (const Row& r : existing.value()) {
          punchline::PayPeriod p = punchline::PayPeriod::from_row(r);
          if (!(ed.value().local_day < p.start_day || sd.value().local_day > p.end_day)) {
            co_return error_response(409, "constraint", "overlaps pay period " + std::to_string(p.id), request_id);
          }
        }
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "period.create", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        auto id = core::next_id(*w.value(), "pay_periods");
        if (!id.ok()) co_return status_response(id.status(), request_id);
        punchline::PayPeriod p;
        p.id = id.value();
        p.start_day = sd.value().local_day;
        p.end_day = ed.value().local_day;
        p.site_zone = zone.value();
        p.tzdb_version = tz.value();
        p.submit_by = submit_by.value();
        p.approve_by = approve_by.value();
        if (Status s = rec.insert("pay_periods", p.to_row()); !s.ok()) co_return status_response(s, request_id);
        // Entries that arrived before the period existed are assigned now.
        int assigned = 0;
        auto entries = w.value()->scan_all("time_entries");
        if (!entries.ok()) co_return status_response(entries.status(), request_id);
        for (const Row& r : entries.value()) {
          punchline::TimeEntry e = punchline::TimeEntry::from_row(r);
          if (e.period_id != 0) continue;
          auto lt = punchline::parse_local_time(e.local_time);
          if (!lt.ok() || lt.value().local_day < p.start_day || lt.value().local_day > p.end_day) continue;
          e.period_id = p.id;
          if (Status s = rec.update("time_entries", e.to_row()); !s.ok()) co_return status_response(s, request_id);
          ++assigned;
        }
        auto shifts = w.value()->scan_all("shifts");
        if (!shifts.ok()) co_return status_response(shifts.status(), request_id);
        for (const Row& r : shifts.value()) {
          punchline::Shift sh = punchline::Shift::from_row(r);
          if (sh.period_id != 0 || sh.local_day < p.start_day || sh.local_day > p.end_day) continue;
          sh.period_id = p.id;
          if (Status s = rec.update("shifts", sh.to_row()); !s.ok()) co_return status_response(s, request_id);
        }
        if (Status s = rec.set_target("pay_periods", std::to_string(p.id)); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        nlohmann::json out = period_json(p);
        out["entries_assigned"] = assigned;
        co_return ok_response(out, request_id, 201);
      },
      {drogon::Get, drogon::Post});

  // ---- Stage 6: the approval lifecycle ------------------------------------

  // submit / approve / release: {employee_number?, note?, override_reason?}.
  // Without employee_number the caller acts for themself (an employee
  // submitting their own timesheet). Standing is PL-7's business.
  app.registerHandler(
      "/api/v1/periods/{id}/{action}",
      [self, mod](drogon::HttpRequestPtr req, std::string id_text, std::string action) -> Handler {
        const std::string request_id = new_request_id();
        std::string to;
        if (action == "submit") to = "submitted";
        else if (action == "approve") to = "approved";
        else if (action == "release") to = "released";
        else if (action == "lock") to = "locked";
        else co_return error_response(404, "not_found", "no such action", request_id);
        auto body = parse_body(req, {"employee_number", "note", "override_reason"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "period.transition");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "period.transition");
        auto period_id = path_id(id_text);
        if (!period_id.ok()) co_return status_response(period_id.status(), request_id);
        auto number = body_string(body.value(), "employee_number", false);
        auto note = body_string(body.value(), "note", false);
        auto override_reason = body_string(body.value(), "override_reason", false);
        for (const Status& s : {number.status(), note.status(), override_reason.status()}) {
          if (!s.ok()) co_return status_response(s, request_id);
        }
        const Caller& c = who.value();
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        core::Recorder rec(*w.value(), self->policy(), c.actor(), "period." + action, self->now_us(), note.value().empty() ? "" : "note given");
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        nlohmann::json out;
        if (to == "locked") {
          auto r = punchline::lock_period(rec, period_id.value(), standing_of(c), c.principal.tid, c.principal.oid, c.account, self->now_us());
          if (!r.ok()) co_return status_response(r.status(), request_id);
          out["employees"] = r.value().employees;
          out["entries"] = r.value().entries;
        } else {
          std::int64_t employee_id = 0;
          if (!number.value().empty()) {
            auto emp = punchline::employee_by_number(*w.value(), number.value());
            if (!emp.ok()) co_return status_response(emp.status(), request_id);
            if (!emp.value().has_value()) co_return error_response(404, "not_found", "no such employee", request_id);
            employee_id = emp.value()->id;
          } else if (c.employee_id) {
            employee_id = *c.employee_id;
          } else {
            co_return error_response(400, "invalid", "employee_number is required", request_id);
          }
          punchline::TransitionRequest tr;
          tr.period_id = period_id.value();
          tr.employee_id = employee_id;
          tr.to_state = to;
          tr.who = standing_of(c, override_reason.value());
          tr.acted_by_tid = c.principal.tid;
          tr.acted_by_oid = c.principal.oid;
          tr.acted_by_account = c.account;
          tr.note = note.value();
          tr.now_us = self->now_us();
          auto r = punchline::transition(rec, tr);
          if (!r.ok()) co_return status_response(r.status(), request_id);
          out["employee_id"] = employee_id;
          out["from_state"] = r.value().from_state;
          out["entries"] = r.value().entries;
          out["shifts"] = r.value().shifts;
          out["approval_id"] = r.value().approval_id;
          out["exceptions_resolved"] = r.value().exceptions_resolved;
          if (Status s = rec.set_target("approvals", std::to_string(r.value().approval_id)); !s.ok()) co_return status_response(s, request_id);
        }
        out["period_id"] = period_id.value();
        out["state"] = to;
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        log(LogLevel::Info, "period." + action,
            {{"request_id", request_id}, {"by", c.describe()}, {"period_id", period_id.value()}, {"override", !override_reason.value().empty()}});
        co_return ok_response(out, request_id);
      },
      {drogon::Post});

  // ---- Stage 6: manual entries and corrections ----------------------------

  app.registerHandler(
      "/api/v1/entries/manual",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"employee_number", "kind", "device_time_us", "local_time", "site_zone", "tzdb_version", "pay_code", "note",
                                     "correction_of", "reason"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "entries.manual");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "entries.manual");
        const Caller& c = who.value();
        auto number = body_string(body.value(), "employee_number");
        auto kind = body_string(body.value(), "kind");
        auto at = body_int(body.value(), "device_time_us");
        auto local = body_string(body.value(), "local_time");
        auto zone = body_string(body.value(), "site_zone");
        auto tz = body_string(body.value(), "tzdb_version");
        auto code = body_string(body.value(), "pay_code", false);
        auto note = body_string(body.value(), "note", false);
        auto correction_of = body_int(body.value(), "correction_of", false);
        auto reason = body_string(body.value(), "reason");
        for (const Status& s : {number.status(), kind.status(), at.status(), local.status(), zone.status(), tz.status(), code.status(),
                                note.status(), correction_of.status(), reason.status()}) {
          if (!s.ok()) co_return status_response(s, request_id);
        }
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto emp = punchline::employee_by_number(*w.value(), number.value());
        if (!emp.ok()) co_return status_response(emp.status(), request_id);
        if (!emp.value().has_value()) co_return error_response(404, "not_found", "no such employee", request_id);
        // Standing: payroll, or the employee's supervisor at the time of the action.
        bool allowed = c.has_role("payroll");
        if (!allowed && c.has_role("supervisor") && c.employee_id) {
          auto sup = punchline::rules::supervisor_at(*w.value(), emp.value()->id, self->now_us());
          if (!sup.ok()) co_return status_response(sup.status(), request_id);
          allowed = sup.value() && *sup.value() == *c.employee_id;
        }
        if (!allowed) co_return error_response(403, "forbidden", "the employee's supervisor or payroll may enter a manual punch", request_id);
        punchline::ManualEntry m;
        m.employee_id = emp.value()->id;
        m.kind = kind.value();
        m.device_time_us = at.value();
        m.local_time = local.value();
        m.site_zone = zone.value();
        m.tzdb_version = tz.value();
        m.pay_code = code.value();
        m.note = note.value();
        m.correction_of = correction_of.value();
        m.reason = reason.value();
        core::Recorder rec(*w.value(), self->policy(), c.actor(), m.correction_of != 0 ? "entry.correct" : "entry.manual", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        auto e = punchline::record_manual_entry(rec, mod->config(), m, self->now_us());
        if (!e.ok()) co_return status_response(e.status(), request_id);
        if (Status s = rec.set_target("time_entries", std::to_string(e.value().id)); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        log(LogLevel::Info, "entry.manual", {{"request_id", request_id}, {"by", c.describe()}, {"entry_id", e.value().id},
                                             {"correction_of", m.correction_of}});
        co_return ok_response(entry_json(e.value()), request_id, 201);
      },
      {drogon::Post});

  // ---- Stage 6: the exception queue ---------------------------------------

  // A supervisor sees their reports' items (and the device items of those
  // reports); payroll and admin see everything; an employee sees their own.
  app.registerHandler(
      "/api/v1/exceptions",
      [self](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "exceptions.list");
        const std::string kind = req->getParameter("kind");
        auto rd = self->store().begin_read();
        if (!rd.ok()) co_return status_response(rd.status(), request_id);
        bool all = false;
        auto scope = scope_of(*rd.value(), who.value(), self->now_us(), all);
        if (!scope.ok()) co_return status_response(scope.status(), request_id);
        auto items = punchline::open_exceptions(*rd.value(), all ? std::vector<std::int64_t>() : scope.value(), kind);
        if (!items.ok()) co_return status_response(items.status(), request_id);
        nlohmann::json list = nlohmann::json::array();
        for (const punchline::Exception& x : items.value()) {
          if (!all && x.employee_id == 0) continue;  // device-only items go to payroll and admin
          list.push_back(exception_json(x));
        }
        co_return ok_response({{"exceptions", list}, {"count", list.size()}}, request_id);
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/exceptions/{id}/{action}",
      [self, mod](drogon::HttpRequestPtr req, std::string id_text, std::string action) -> Handler {
        const std::string request_id = new_request_id();
        if (action != "resolve" && action != "dismiss") co_return error_response(404, "not_found", "no such action", request_id);
        auto body = parse_body(req, {"note"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "exceptions.close");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "exceptions.close");
        auto id = path_id(id_text);
        if (!id.ok()) co_return status_response(id.status(), request_id);
        auto note = body_string(body.value(), "note", false);
        if (!note.ok()) co_return status_response(note.status(), request_id);
        const Caller& c = who.value();
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        auto x = punchline::exception_by_id(*w.value(), id.value());
        if (!x.ok()) co_return status_response(x.status(), request_id);
        if (!x.value().has_value()) co_return error_response(404, "not_found", "no such exception", request_id);
        bool all = false;
        auto scope = scope_of(*w.value(), c, self->now_us(), all);
        if (!scope.ok()) co_return status_response(scope.status(), request_id);
        // An employee may not close their own items; their supervisor, payroll or admin may.
        const bool own = c.employee_id && x.value()->employee_id == *c.employee_id && !c.has_role("supervisor");
        if (own || !in_scope(scope.value(), all, x.value()->employee_id) || (x.value()->employee_id == 0 && !all)) {
          co_return error_response(403, "forbidden", "not your queue", request_id);
        }
        core::Recorder rec(*w.value(), self->policy(), c.actor(), "exception." + action, self->now_us(), note.value().empty() ? "" : "note given");
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        if (Status s = punchline::close_exception(rec, id.value(), action == "resolve" ? "resolved" : "dismissed", c.describe(), self->now_us());
            !s.ok()) {
          co_return status_response(s, request_id);
        }
        if (Status s = rec.set_target("exceptions", std::to_string(id.value())); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        co_return ok_response({{"id", id.value()}, {"state", action == "resolve" ? "resolved" : "dismissed"}}, request_id);
      },
      {drogon::Post});

  // ---- Stage 6: employee self-service --------------------------------------

  // "Your approver: <name>", with a flag action that opens a ticket row.
  app.registerHandler(
      "/api/v1/me",
      [self](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "me");
        const Caller& c = who.value();
        if (!c.employee_id) co_return error_response(404, "not_found", "no employee row for this credential", request_id);
        auto rd = self->store().begin_read();
        if (!rd.ok()) co_return status_response(rd.status(), request_id);
        auto emp = punchline::employee_by_id(*rd.value(), *c.employee_id);
        if (!emp.ok() || !emp.value().has_value()) co_return error_response(404, "not_found", "employee vanished", request_id);
        nlohmann::json out;
        out["employee"] = employee_json(*emp.value());
        out["roles"] = c.roles;
        auto sup = punchline::rules::supervisor_at(*rd.value(), *c.employee_id, self->now_us());
        if (!sup.ok()) co_return status_response(sup.status(), request_id);
        if (sup.value()) {
          auto s = punchline::employee_by_id(*rd.value(), *sup.value());
          if (s.ok() && s.value().has_value()) {
            out["approver"] = {{"employee_number", s.value()->employee_number}, {"display_name", s.value()->display_name}};
          }
        } else {
          out["approver"] = nullptr;
        }
        auto open = punchline::open_exceptions(*rd.value(), {*c.employee_id});
        if (!open.ok()) co_return status_response(open.status(), request_id);
        out["open_exceptions"] = open.value().size();
        auto shift = punchline::open_shift_of(*rd.value(), *c.employee_id);
        if (!shift.ok()) co_return status_response(shift.status(), request_id);
        out["clocked_in"] = shift.value().has_value();
        if (shift.value()) out["open_shift"] = shift_json(*shift.value());
        co_return ok_response(out, request_id);
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/me/flag",
      [self, mod](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"note"});
        if (!body.ok()) co_return co_await body_rejected(self, mod, req, request_id, body.status(), "me.flag");
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "me.flag");
        const Caller& c = who.value();
        if (!c.employee_id) co_return error_response(404, "not_found", "no employee row for this credential", request_id);
        auto note = body_string(body.value(), "note");
        if (!note.ok()) co_return status_response(note.status(), request_id);
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        core::Recorder rec(*w.value(), self->policy(), c.actor(), "approver.flag", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        punchline::ExceptionKey key;
        key.kind = "approver_flag";
        key.employee_id = *c.employee_id;
        auto o = punchline::open_exception(rec, key, note.value().substr(0, 512), self->now_us());
        if (!o.ok()) co_return status_response(o.status(), request_id);
        if (Status s = rec.set_target("exceptions", std::to_string(o.value().id)); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        co_return ok_response({{"exception_id", o.value().id}, {"created", o.value().created}}, request_id, o.value().created ? 201 : 200);
      },
      {drogon::Post});

  // An employee's, a report's, or anyone's (payroll) period: entries, shifts, state.
  app.registerHandler(
      "/api/v1/timesheets/{period}/{employee_number}",
      [self](drogon::HttpRequestPtr req, std::string period_text, std::string number) -> Handler {
        const std::string request_id = new_request_id();
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "timesheet");
        auto period_id = path_id(period_text);
        if (!period_id.ok()) co_return status_response(period_id.status(), request_id);
        auto rd = self->store().begin_read();
        if (!rd.ok()) co_return status_response(rd.status(), request_id);
        auto emp = number == "me" && who.value().employee_id ? punchline::employee_by_id(*rd.value(), *who.value().employee_id)
                                                              : punchline::employee_by_number(*rd.value(), number);
        if (!emp.ok()) co_return status_response(emp.status(), request_id);
        if (!emp.value().has_value()) co_return error_response(404, "not_found", "no such employee", request_id);
        bool all = false;
        auto scope = scope_of(*rd.value(), who.value(), self->now_us(), all);
        if (!scope.ok()) co_return status_response(scope.status(), request_id);
        if (!in_scope(scope.value(), all, emp.value()->id)) co_return error_response(403, "forbidden", "not your timesheet", request_id);
        auto period = punchline::period_by_id(*rd.value(), period_id.value());
        if (!period.ok()) co_return status_response(period.status(), request_id);
        if (!period.value().has_value()) co_return error_response(404, "not_found", "no such pay period", request_id);
        auto entries = punchline::entries_in_period(*rd.value(), period_id.value(), emp.value()->id);
        auto shifts = punchline::shifts_in_period(*rd.value(), period_id.value(), emp.value()->id);
        if (!entries.ok()) co_return status_response(entries.status(), request_id);
        if (!shifts.ok()) co_return status_response(shifts.status(), request_id);
        nlohmann::json out;
        out["period"] = period_json(*period.value());
        out["employee"] = employee_json(*emp.value());
        out["entries"] = nlohmann::json::array();
        std::map<std::string, int> states;
        int device_attested = 0;
        for (const punchline::TimeEntry& e : entries.value()) {
          out["entries"].push_back(entry_json(e));
          if (e.superseded_by == 0) {
            ++states[e.state];
            if (e.attestation == "device") ++device_attested;
          }
        }
        out["shifts"] = nlohmann::json::array();
        std::int64_t total_us = 0;
        for (const punchline::Shift& s : shifts.value()) {
          out["shifts"].push_back(shift_json(s));
          if (s.duration_us) total_us += *s.duration_us;
        }
        out["states"] = states;
        out["device_attested_entries"] = device_attested;
        out["total_hours_hundredths"] = total_us / 36'000'000;
        auto open = punchline::open_exceptions(*rd.value(), {emp.value()->id});
        if (open.ok()) {
          out["open_exceptions"] = nlohmann::json::array();
          for (const punchline::Exception& x : open.value()) out["open_exceptions"].push_back(exception_json(x));
        }
        co_return ok_response(out, request_id);
      },
      {drogon::Get});

  // ---- Stage 6: the supervisor queue ---------------------------------------

  app.registerHandler(
      "/api/v1/supervisor/periods/{id}",
      [self](drogon::HttpRequestPtr req, std::string id_text) -> Handler {
        const std::string request_id = new_request_id();
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "supervisor.queue");
        const Caller& c = who.value();
        if (!c.has_role("supervisor") || !c.employee_id) co_return error_response(403, "forbidden", "supervisor role required", request_id);
        auto period_id = path_id(id_text);
        if (!period_id.ok()) co_return status_response(period_id.status(), request_id);
        auto rd = self->store().begin_read();
        if (!rd.ok()) co_return status_response(rd.status(), request_id);
        auto period = punchline::period_by_id(*rd.value(), period_id.value());
        if (!period.ok()) co_return status_response(period.status(), request_id);
        if (!period.value().has_value()) co_return error_response(404, "not_found", "no such pay period", request_id);
        auto reports = punchline::reports_of(*rd.value(), *c.employee_id, self->now_us());
        if (!reports.ok()) co_return status_response(reports.status(), request_id);
        nlohmann::json out;
        out["period"] = period_json(*period.value());
        out["reports"] = nlohmann::json::array();
        for (std::int64_t employee : reports.value()) {
          auto emp = punchline::employee_by_id(*rd.value(), employee);
          if (!emp.ok() || !emp.value().has_value()) continue;
          auto entries = punchline::entries_in_period(*rd.value(), period_id.value(), employee);
          if (!entries.ok()) co_return status_response(entries.status(), request_id);
          std::map<std::string, int> states;
          int device_attested = 0;
          for (const punchline::TimeEntry& e : entries.value()) {
            if (e.superseded_by != 0) continue;
            ++states[e.state];
            if (e.attestation == "device") ++device_attested;
          }
          auto open = punchline::open_exceptions(*rd.value(), {employee});
          nlohmann::json j;
          j["employee"] = employee_json(*emp.value());
          j["states"] = states;
          j["device_attested_entries"] = device_attested;
          j["awaiting_approval"] = states.count("submitted") != 0;
          j["open_exceptions"] = open.ok() ? open.value().size() : 0;
          out["reports"].push_back(j);
        }
        co_return ok_response(out, request_id);
      },
      {drogon::Get});

  // ---- Stage 6: payroll -----------------------------------------------------

  // Hours by employee by pay code for a period: released and locked shifts
  // only. Nothing below released is in this answer, whatever its state.
  // `?format=csv` for the export.
  app.registerHandler(
      "/api/v1/payroll/periods/{id}",
      [self](drogon::HttpRequestPtr req, std::string id_text) -> Handler {
        const std::string request_id = new_request_id();
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "payroll.period");
        if (!who.value().has_role("payroll")) co_return error_response(403, "forbidden", "payroll role required", request_id);
        auto period_id = path_id(id_text);
        if (!period_id.ok()) co_return status_response(period_id.status(), request_id);
        auto rd = self->store().begin_read();
        if (!rd.ok()) co_return status_response(rd.status(), request_id);
        auto period = punchline::period_by_id(*rd.value(), period_id.value());
        if (!period.ok()) co_return status_response(period.status(), request_id);
        if (!period.value().has_value()) co_return error_response(404, "not_found", "no such pay period", request_id);
        auto shifts = rd.value()->scan_all("shifts", "shifts_period", engine::Bound{{Value::integer(period_id.value())}},
                                           engine::Bound{{Value::integer(period_id.value())}});
        if (!shifts.ok()) co_return status_response(shifts.status(), request_id);
        // employee -> pay code -> microseconds
        std::map<std::int64_t, std::map<std::string, std::int64_t>> hours;
        std::map<std::int64_t, int> shift_counts;
        int withheld = 0;
        for (const Row& r : shifts.value()) {
          punchline::Shift s = punchline::Shift::from_row(r);
          if (s.state != "released" && s.state != "locked") {
            ++withheld;
            continue;
          }
          if (!s.duration_us) continue;
          hours[s.employee_id][s.pay_code.empty() ? "regular" : s.pay_code] += *s.duration_us;
          ++shift_counts[s.employee_id];
        }
        const bool csv = req->getParameter("format") == "csv";
        nlohmann::json out;
        out["period"] = period_json(*period.value());
        out["employees"] = nlohmann::json::array();
        std::string text = "employee_number,display_name,pay_code,hours_hundredths,shifts\n";
        for (const auto& [employee, codes] : hours) {
          auto emp = punchline::employee_by_id(*rd.value(), employee);
          if (!emp.ok() || !emp.value().has_value()) continue;
          nlohmann::json j;
          j["employee"] = employee_json(*emp.value());
          j["shifts"] = shift_counts[employee];
          j["hours_hundredths_by_pay_code"] = nlohmann::json::object();
          for (const auto& [code, us] : codes) {
            j["hours_hundredths_by_pay_code"][code] = us / 36'000'000;
            text += csv_field(emp.value()->employee_number) + "," + csv_field(emp.value()->display_name) + "," + csv_field(code) + "," +
                    std::to_string(us / 36'000'000) + "," + std::to_string(shift_counts[employee]) + "\n";
          }
          out["employees"].push_back(j);
        }
        out["shifts_withheld_not_released"] = withheld;
        if (csv) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          resp->setContentTypeString("text/csv; charset=utf-8");
          resp->addHeader("X-Request-Id", request_id);
          resp->setBody(text);
          co_return resp;
        }
        co_return ok_response(out, request_id);
      },
      {drogon::Get});

  // The period audit report: every approval, override, manual entry and
  // correction, exception resolution and lock, with actor and timestamp,
  // straight from the audit log and the rows that reference it.
  app.registerHandler(
      "/api/v1/payroll/periods/{id}/audit",
      [self](drogon::HttpRequestPtr req, std::string id_text) -> Handler {
        const std::string request_id = new_request_id();
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "payroll.audit");
        if (!who.value().has_role("payroll") && !who.value().has_role("admin")) {
          co_return error_response(403, "forbidden", "payroll or admin role required", request_id);
        }
        auto period_id = path_id(id_text);
        if (!period_id.ok()) co_return status_response(period_id.status(), request_id);
        auto rd = self->store().begin_read();
        if (!rd.ok()) co_return status_response(rd.status(), request_id);
        auto period = punchline::period_by_id(*rd.value(), period_id.value());
        if (!period.ok()) co_return status_response(period.status(), request_id);
        if (!period.value().has_value()) co_return error_response(404, "not_found", "no such pay period", request_id);
        auto audit_of = [&](std::int64_t audit_id) -> nlohmann::json {
          auto row = rd.value()->get(core::kAuditLog, {Value::integer(audit_id)});
          nlohmann::json j;
          j["audit_id"] = audit_id;
          if (!row.ok() || !row.value().has_value()) return j;
          const Row& a = *row.value();
          j["at_us"] = a[core::audit::kAt].as_int64();
          j["actor_kind"] = a[core::audit::kActorKind].as_text();
          if (!a[core::audit::kActorTid].is_null()) j["actor_tid"] = a[core::audit::kActorTid].as_text();
          if (!a[core::audit::kActorOid].is_null()) j["actor_oid"] = a[core::audit::kActorOid].as_text();
          if (!a[core::audit::kActorAccount].is_null()) j["actor_account"] = a[core::audit::kActorAccount].as_text();
          if (!a[core::audit::kActorDevice].is_null()) j["actor_device"] = a[core::audit::kActorDevice].to_string();
          j["action"] = a[core::audit::kAction].as_text();
          if (!a[core::audit::kRequestId].is_null()) j["request_id"] = a[core::audit::kRequestId].as_text();
          return j;
        };
        nlohmann::json out;
        out["period"] = period_json(*period.value());
        out["transitions"] = nlohmann::json::array();
        auto approvals = rd.value()->scan_all("approvals", "approvals_period", engine::Bound{{Value::integer(period_id.value())}},
                                              engine::Bound{{Value::integer(period_id.value())}});
        if (!approvals.ok()) co_return status_response(approvals.status(), request_id);
        for (const Row& r : approvals.value()) {
          punchline::Approval a = punchline::Approval::from_row(r);
          nlohmann::json j = audit_of(a.audit_id);
          j["employee_id"] = a.employee_id;
          j["from_state"] = a.from_state;
          j["to_state"] = a.to_state;
          j["acted_at_us"] = a.acted_at;
          j["override"] = a.note.rfind("override:", 0) == 0;
          if (!a.note.empty()) j["note"] = a.note;
          out["transitions"].push_back(j);
        }
        out["manual_entries"] = nlohmann::json::array();
        out["exceptions"] = nlohmann::json::array();
        auto entries = rd.value()->scan_all("time_entries", "time_entries_period", engine::Bound{{Value::integer(period_id.value())}},
                                            engine::Bound{{Value::integer(period_id.value())}});
        if (!entries.ok()) co_return status_response(entries.status(), request_id);
        std::set<std::int64_t> employees;
        for (const Row& r : entries.value()) {
          punchline::TimeEntry e = punchline::TimeEntry::from_row(r);
          employees.insert(e.employee_id);
          if (e.attestation == "manual") {
            nlohmann::json j = entry_json(e);
            j["reason"] = e.correction_reason;
            out["manual_entries"].push_back(j);
          }
        }
        auto exceptions = rd.value()->scan_all("exceptions");
        if (!exceptions.ok()) co_return status_response(exceptions.status(), request_id);
        for (const Row& r : exceptions.value()) {
          punchline::Exception x = punchline::Exception::from_row(r);
          const bool in_period = x.period_id == period_id.value() ||
                                 (x.period_id == 0 && x.employee_id != 0 && employees.count(x.employee_id) && x.opened_at >= 0);
          if (!in_period) continue;
          nlohmann::json j = exception_json(x);
          if (x.resolution_audit_id != 0) j["resolution"] = audit_of(x.resolution_audit_id);
          out["exceptions"].push_back(j);
        }
        co_return ok_response(out, request_id);
      },
      {drogon::Get});

  // ---- Stage 6: segregation of duties ---------------------------------------

  // Principals holding both supervisor and payroll: a finding, listed.
  app.registerHandler(
      "/api/v1/admin/roles/conflicts",
      [self](drogon::HttpRequestPtr req) -> Handler {
        const std::string request_id = new_request_id();
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return auth_error(who, request_id, "roles.conflicts");
        if (!who.value().has_role("admin") && !who.value().has_role("payroll")) {
          co_return error_response(403, "forbidden", "admin or payroll role required", request_id);
        }
        auto rd = self->store().begin_read();
        if (!rd.ok()) co_return status_response(rd.status(), request_id);
        auto grants = rd.value()->scan_all(core::kRoleGrants);
        if (!grants.ok()) co_return status_response(grants.status(), request_id);
        std::map<std::pair<std::string, std::string>, std::set<std::string>> by_principal;
        for (const Row& r : grants.value()) {
          by_principal[{r[core::roles::kTid].as_text(), r[core::roles::kOid].as_text()}].insert(r[core::roles::kRole].as_text());
        }
        nlohmann::json out;
        out["conflicts"] = nlohmann::json::array();
        for (const auto& [principal, roles] : by_principal) {
          if (roles.count("supervisor") && roles.count("payroll")) {
            nlohmann::json j;
            j["tid"] = principal.first;
            j["oid"] = principal.second;
            j["roles"] = roles;
            auto emp = punchline::employee_by_identity(*rd.value(), principal.first, principal.second);
            if (emp.ok() && emp.value().has_value()) j["employee"] = employee_json(*emp.value());
            out["conflicts"].push_back(j);
          }
        }
        out["principals_checked"] = by_principal.size();
        co_return ok_response(out, request_id);
      },
      {drogon::Get});
}

}  // namespace archivum::server

namespace archivum::server {

std::vector<ServerModule> builtin_modules() {
  auto state = std::make_shared<PunchlineModule>();
  ServerModule m;
  m.data = &punchline::module();
  m.configure = [state](const nlohmann::json& section) { return state->configure(section); };
  m.register_routes = [state](App& app) { state->register_routes(app); };
  m.start = [state](App& app) { state->start(app); };
  m.stop = [state] { state->stop(); };
  m.run_once = [state](App& app) { return state->monitor_once(app); };
  return {m};
}

}  // namespace archivum::server
