#include "archivum/server/identity.h"

#include <algorithm>
#include <cctype>

#include "archivum/core/accounts.h"
#include "archivum/core/authz.h"
#include "archivum/core/credentials.h"
#include "archivum/punchline/data.h"
#include "archivum/server/log.h"

namespace archivum::server {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Status codes carry the HTTP meaning: NotFound/InvalidArgument = 401,
// Constraint (a known credential without standing) = 403.
Status unauthorized(const std::string& reason) { return Status::invalid_argument(reason); }
Status forbidden(const std::string& reason) { return Status::constraint(reason); }

std::string base64_decode(const std::string& in) {
  static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=') break;
    const auto pos = alphabet.find(c);
    if (pos == std::string::npos) return "";
    acc = (acc << 6) | static_cast<std::uint32_t>(pos);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return out;
}

}  // namespace

core::Actor Caller::actor() const {
  core::Actor a;
  a.request_id = request_id;
  switch (kind) {
    case Kind::Principal:
      a.kind = core::Actor::Kind::Principal;
      a.tid = principal.tid;
      a.oid = principal.oid;
      break;
    case Kind::Device:
      a.kind = core::Actor::Kind::Device;
      a.device = device_uuid;
      break;
    case Kind::Account:
      a.kind = core::Actor::Kind::Account;
      a.account = account;
      break;
  }
  return a;
}

std::string Caller::describe() const {
  switch (kind) {
    case Kind::Principal:
      return "principal " + principal.tid + "/" + principal.oid;
    case Kind::Device:
      return "device " + core::uuid_to_string(device_uuid);
    case Kind::Account:
      return "account " + account;
  }
  return "?";
}

Authenticator::Authenticator(engine::Store& store, std::shared_ptr<OidcValidator> validator, const core::RecordPolicy& policy)
    : store_(store), validator_(std::move(validator)), policy_(policy) {}

AuthFailure Authenticator::failure_of(const Status& s) {
  AuthFailure f;
  f.reason = s.to_string();
  f.http_status = s.code() == ErrorCode::Constraint ? 403 : 401;
  return f;
}

void Authenticator::audit_failure(const core::Actor& actor, const std::string& action, const std::string& detail) {
  auto w = store_.begin_write();
  if (!w.ok()) return;
  core::Recorder rec(*w.value(), policy_, actor, action, store_.db().now_us(), detail);
  if (rec.status().ok()) (void)w.value()->commit();
}

drogon::Task<Result<Caller>> Authenticator::authenticate(drogon::HttpRequestPtr req, std::string request_id) {
  const std::string header = req->getHeader("authorization");
  const auto space = header.find(' ');
  const std::string scheme = lower(header.substr(0, space));
  std::string rest = space == std::string::npos ? "" : header.substr(space + 1);
  while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
  if (scheme == "bearer") {
    auto p = co_await validator_->authenticate(header);
    if (!p.ok()) co_return unauthorized(p.status().to_string());
    co_return resolve_principal(p.value(), request_id);
  }
  if (scheme == "device") co_return resolve_device(rest, request_id);
  if (scheme == "basic") co_return resolve_account(rest, request_id);
  co_return unauthorized("no credential");
}

Result<Caller> Authenticator::resolve_principal(const Principal& p, const std::string& request_id) {
  Caller c;
  c.kind = Caller::Kind::Principal;
  c.request_id = request_id;
  c.principal = p;
  auto rd = store_.begin_read();
  if (!rd.ok()) return rd.status();
  auto roles = core::roles_for(*rd.value(), p.tid, p.oid);
  if (!roles.ok()) return roles.status();
  c.roles = roles.value();
  auto emp = punchline::employee_by_identity(*rd.value(), p.tid, p.oid);
  if (!emp.ok()) return emp.status();
  if (emp.value().has_value()) {
    if (!emp.value()->active) {
      rd.value().reset();
      audit_failure(c.actor(), "auth.inactive_employee", "employee " + std::to_string(emp.value()->id));
      log(LogLevel::Alert, "auth.inactive_employee", {{"request_id", request_id}, {"tid", p.tid}, {"oid", p.oid}});
      return forbidden("employee inactive");
    }
    c.employee_id = emp.value()->id;
  }
  rd.value().reset();
  if (c.roles.empty() && !c.employee_id) {
    // The nullable-oid rule: a valid token for an identity nobody mapped is
    // refused and recorded; email is never consulted.
    audit_failure(c.actor(), "auth.unknown_principal", "no role and no employee for this oid");
    log(LogLevel::Alert, "auth.unknown_principal", {{"request_id", request_id}, {"tid", p.tid}, {"oid", p.oid}});
    return forbidden("unknown principal");
  }
  return c;
}

Result<Caller> Authenticator::resolve_device(const std::string& credential, const std::string& request_id) {
  const auto colon = credential.find(':');
  if (colon == std::string::npos) return unauthorized("malformed device credential");
  auto uuid = core::uuid_from_string(credential.substr(0, colon));
  if (!uuid.ok()) return unauthorized("malformed device id");
  const std::string secret = credential.substr(colon + 1);
  Caller c;
  c.kind = Caller::Kind::Device;
  c.request_id = request_id;
  c.device_uuid = uuid.value();
  auto rd = store_.begin_read();
  if (!rd.ok()) return rd.status();
  auto dev = punchline::device_by_uuid(*rd.value(), uuid.value());
  if (!dev.ok()) return dev.status();
  if (!dev.value().has_value()) {
    rd.value().reset();
    log(LogLevel::Warn, "auth.device_unknown", {{"request_id", request_id}, {"device", credential.substr(0, colon)}});
    return unauthorized("unknown device");
  }
  const punchline::Device& d = *dev.value();
  rd.value().reset();
  if (!core::verify_secret(d.credential_hash, secret)) {
    audit_failure(c.actor(), "auth.device_bad_credential", "");
    log(LogLevel::Alert, "auth.device_bad_credential", {{"request_id", request_id}, {"device", core::uuid_to_string(d.device_uuid)}});
    return unauthorized("bad device credential");
  }
  if (d.revoked_at != 0) {
    audit_failure(c.actor(), "auth.device_revoked", "");
    log(LogLevel::Alert, "auth.device_revoked", {{"request_id", request_id}, {"device", core::uuid_to_string(d.device_uuid)}});
    return forbidden("device revoked");
  }
  c.device_id = d.id;
  c.employee_id = d.employee_id;  // from the enrollment record, never the request
  return c;
}

Result<Caller> Authenticator::resolve_account(const std::string& basic_b64, const std::string& request_id) {
  const std::string decoded = base64_decode(basic_b64);
  const auto colon = decoded.find(':');
  if (colon == std::string::npos) return unauthorized("malformed basic credential");
  const std::string username = decoded.substr(0, colon);
  const std::string password = decoded.substr(colon + 1);
  auto acc = core::verify_account(store_, policy_, username, password, request_id, store_.db().now_us());
  if (!acc.ok()) {
    core::Actor a;
    a.kind = core::Actor::Kind::Account;
    a.account = username;
    a.request_id = request_id;
    audit_failure(a, "auth.account_failed", acc.status().to_string());
    log(LogLevel::Alert, "auth.account_failed", {{"request_id", request_id}, {"account", username}, {"reason", acc.status().to_string()}});
    return unauthorized("account refused");
  }
  Caller c;
  c.kind = Caller::Kind::Account;
  c.request_id = request_id;
  c.account = username;
  c.account_kind = acc.value().kind;
  c.roles = acc.value().kind == "break_glass" ? std::set<std::string>{"admin"} : std::set<std::string>{"device_admin"};
  log(LogLevel::Alert, "auth.account_used", {{"request_id", request_id}, {"account", username}, {"kind", acc.value().kind}});
  return c;
}

}  // namespace archivum::server
