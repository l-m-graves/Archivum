// Who is calling, from three credentials:
//   Bearer   an Entra token (OIDC); the principal's roles come from
//            role_grants and its employee row from the Punchline module.
//            A principal with neither is unknown: 403, audited.
//   Device   "Device <uuid>:<secret>", the per-device credential from
//            enrollment; the employee is the enrollment record's, never
//            anything the request says.
//   Basic    a local account (break-glass or device administration);
//            every use is audited and logged at alert level.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/utils/coroutine.h>

#include "archivum/core/recorder.h"
#include "archivum/engine/store.h"
#include "archivum/server/oidc.h"
#include "archivum/status.h"

namespace archivum::server {

struct Caller {
  enum class Kind { Principal, Device, Account };
  Kind kind = Kind::Principal;
  std::string request_id;
  // Principal
  Principal principal;
  std::set<std::string> roles;
  std::optional<std::int64_t> employee_id;  // Principal with an employee row, or Device
  // Device
  std::int64_t device_id = 0;
  engine::UuidBytes device_uuid{};
  // Account
  std::string account;
  std::string account_kind;

  core::Actor actor() const;
  bool has_role(const std::string& role) const { return roles.count(role) != 0; }
  std::string describe() const;  // for logs: never a secret
};

// A refused authentication: the HTTP status to answer with and the
// WWW-Authenticate challenge, with the reason logged, never returned.
struct AuthFailure {
  int http_status = 401;
  std::string challenge;  // "Bearer", "Device", "Basic"
  std::string reason;
};

class Authenticator {
 public:
  Authenticator(engine::Store& store, std::shared_ptr<OidcValidator> validator, const core::RecordPolicy& policy);

  // Chooses by the Authorization scheme. `require_roles`: at least one of
  // them, for principals and accounts; devices never hold roles.
  drogon::Task<Result<Caller>> authenticate(drogon::HttpRequestPtr req, std::string request_id);

  // The failure the last non-ok result carries, for the response.
  static AuthFailure failure_of(const Status& s);

 private:
  Result<Caller> resolve_principal(const Principal& p, const std::string& request_id);
  Result<Caller> resolve_device(const std::string& credential, const std::string& request_id);
  Result<Caller> resolve_account(const std::string& basic_b64, const std::string& request_id);
  void audit_failure(const core::Actor& actor, const std::string& action, const std::string& detail);

  engine::Store& store_;
  std::shared_ptr<OidcValidator> validator_;
  const core::RecordPolicy& policy_;
};

}  // namespace archivum::server
