// Wires configuration, TLS, the store, authentication, the recorder, the
// archive shipper, modules and routes into Drogon's application singleton.
//
// Core routes:
//   GET  /healthz                 no auth; status, TLS expiry, JWKS, schema,
//                                 off-host archive state (503 when failing)
//   GET  /api/v1/whoami           bearer; identity, roles, employee id
//   POST /api/v1/admin/roles      admin; grant {tid, oid, role}
//   POST /api/v1/admin/roles/{id}/revoke
// Module routes: server/src/modules/*.cpp.
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "archivum/core/recorder.h"
#include "archivum/engine/store.h"
#include "archivum/server/archive.h"
#include "archivum/server/config.h"
#include "archivum/server/identity.h"
#include "archivum/server/log.h"
#include "archivum/server/modules.h"
#include "archivum/server/oidc.h"
#include "archivum/status.h"
#include "archivum/vfs.h"

namespace archivum::server {

struct CertificateInfo {
  std::string subject;
  std::chrono::system_clock::time_point not_after;
};
Result<CertificateInfo> read_certificate_info(const std::string& pem_path);

std::vector<std::pair<std::string, std::string>> tls_conf_commands(const TlsConfig& tls);

class App {
 public:
  App(Config config, std::vector<ServerModule> modules = builtin_modules());
  ~App();

  // Opens the store, applies migrations, registers routes and the
  // listener. Does not fetch anything and does not start the shipper.
  Status configure();
  drogon::Task<Status> initialize_auth();
  // Blocks in drogon::app().run(). Starts the archive shipper once
  // authentication is initialised.
  Status run(std::function<void()> ready = {});

  std::shared_ptr<OidcValidator> validator() const { return validator_; }
  const Config& config() const { return config_; }
  engine::Store& store() { return *store_; }
  Authenticator& authenticator() { return *auth_; }
  const core::RecordPolicy& policy() const { return policy_; }
  ArchiveShipper& shipper() { return *shipper_; }
  std::int64_t now_us() const { return store_->db().now_us(); }
  // Test hook: a deterministic clock for the store.
  void set_clock_for_test(std::int64_t (*clock)()) { clock_ = clock; }

 private:
  Config config_;
  std::vector<ServerModule> modules_;
  std::unique_ptr<Vfs> vfs_;
  std::unique_ptr<engine::Store> store_;
  std::shared_ptr<OidcValidator> validator_;
  std::unique_ptr<Authenticator> auth_;
  std::unique_ptr<ArchiveShipper> shipper_;
  core::RecordPolicy policy_;
  CertificateInfo cert_;
  Status init_status_;
  std::int64_t (*clock_)() = nullptr;
};

}  // namespace archivum::server
