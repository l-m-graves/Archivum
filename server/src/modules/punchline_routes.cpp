// Punchline routes for Stage 5: employees, device enrollment and
// revocation, supervisor assignments (admin), and the device heartbeat
// (device credential). Punches and sync arrive with Stage 6.
//
// Every mutating route: parse the body (unknown keys and `employee_id`
// refused), authenticate, check the module rules, mutate through the
// recorder, commit. The employee behind a device request is the
// enrollment record's, never the request's.
#include <drogon/drogon.h>

#include "../http/request.h"
#include "archivum/core/credentials.h"
#include "archivum/core/ids.h"
#include "archivum/punchline/data.h"
#include "archivum/punchline/rules.h"
#include "archivum/punchline/schema.h"
#include "archivum/server/app.h"
#include "archivum/server/modules.h"

namespace archivum::server {
namespace {

using engine::Value;

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
  return j;
}

void register_routes(App& app_ref) {
  App* self = &app_ref;
  auto& app = drogon::app();

  // Employees (admin).
  app.registerHandler(
      "/api/v1/admin/employees",
      [self](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
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
        if (!body.ok()) co_return status_response(body.status(), request_id);
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

  // Device enrollment (admin or device_admin): one device, one credential,
  // one employee. The credential is returned once and never stored.
  app.registerHandler(
      "/api/v1/admin/devices",
      [self](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
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
        if (!body.ok()) co_return status_response(body.status(), request_id);
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
        log(LogLevel::Info, "device.enrolled", {{"request_id", request_id}, {"by", who.value().describe()}, {"device_id", d.id}, {"employee_id", d.employee_id}});
        nlohmann::json out = device_json(d);
        out["credential"] = core::uuid_to_string(d.device_uuid) + ":" + secret;  // shown once
        co_return ok_response(out, request_id, 201);
      },
      {drogon::Get, drogon::Post});

  app.registerHandler(
      "/api/v1/admin/devices/{uuid}/revoke",
      [self](drogon::HttpRequestPtr req, std::string uuid_text) -> drogon::Task<drogon::HttpResponsePtr> {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"reason"});
        if (!body.ok()) co_return status_response(body.status(), request_id);
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

  // Supervisor assignments (admin), effective-dated.
  app.registerHandler(
      "/api/v1/admin/supervisors",
      [self](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"employee_number", "supervisor_employee_number", "effective_from_us", "effective_to_us"});
        if (!body.ok()) co_return status_response(body.status(), request_id);
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
        engine::Row row = {Value::integer(id.value()), Value::integer(emp.value()->id), Value::integer(sup.value()->id),
                           Value::timestamp(from.value()), to.value() == 0 ? Value::null() : Value::timestamp(to.value()),
                           Value::text(who.value().describe()), Value::integer(rec.audit_id())};
        if (Status s = rec.insert("supervisor_assignments", row); !s.ok()) co_return status_response(s, request_id);
        if (Status s = rec.set_target("supervisor_assignments", std::to_string(id.value())); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        co_return ok_response({{"id", id.value()}}, request_id, 201);
      },
      {drogon::Post});

  // Device heartbeat: the first device-authenticated route. The employee
  // in the answer is the enrollment's.
  app.registerHandler(
      "/api/v1/device/heartbeat",
      [self](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"client_time_us", "last_journal_sequence"});
        if (!body.ok()) co_return status_response(body.status(), request_id);
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
        // last_seen is operational state, not an audited change: plain update.
        if (Status s = w.value()->update("devices", d.to_row()); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        nlohmann::json out;
        out["employee"]["display_name"] = emp.value()->display_name;
        out["employee"]["employee_number"] = emp.value()->employee_number;
        out["employee"]["site_zone"] = emp.value()->site_zone;
        out["server_time_us"] = self->now_us();
        if (client_time.value() != 0) out["clock_divergence_us"] = self->now_us() - client_time.value();
        co_return ok_response(out, request_id);
      },
      {drogon::Post});
}

}  // namespace

std::vector<ServerModule> builtin_modules() {
  return {ServerModule{&punchline::module(), &register_routes}};
}

}  // namespace archivum::server
