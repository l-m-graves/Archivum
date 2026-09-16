// Wires configuration, TLS, authentication, and routes into Drogon's
// application singleton. Stage 1 exposes two routes:
//   GET /healthz          no auth; status, certificate expiry, JWKS state
//   GET /api/v1/whoami    bearer auth; the caller's identity claims
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "archivum/server/config.h"
#include "archivum/server/oidc.h"
#include "archivum/status.h"

namespace archivum::server {

struct CertificateInfo {
  std::string subject;
  std::chrono::system_clock::time_point not_after;
};
Result<CertificateInfo> read_certificate_info(const std::string& pem_path);

// The TLS configuration commands applied to the listener. Exposed so tests
// can assert what the policy asks for.
std::vector<std::pair<std::string, std::string>> tls_conf_commands(const TlsConfig& tls);

class App {
 public:
  explicit App(Config config);

  // Registers routes and the listener. Does not fetch anything.
  Status configure();
  // Fetches OIDC discovery and JWKS synchronously on the app loop. Called
  // from within run_hook (the loop must be running).
  drogon::Task<Status> initialize_auth();
  // Blocks in drogon::app().run(). `ready` is invoked on the app loop once
  // authentication has been initialised; if initialisation fails the app
  // quits and run() returns that status.
  Status run(std::function<void()> ready = {});

  std::shared_ptr<OidcValidator> validator() const { return validator_; }
  const Config& config() const { return config_; }

 private:
  Config config_;
  std::shared_ptr<OidcValidator> validator_;
  CertificateInfo cert_;
  Status init_status_;
};

}  // namespace archivum::server
