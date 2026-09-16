#include "archivum/server/app.h"

#include <atomic>
#include <ctime>

#include <drogon/drogon.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "../src/http/json_bridge.h"

namespace archivum::server {

Result<CertificateInfo> read_certificate_info(const std::string& pem_path) {
  BIO* bio = BIO_new_file(pem_path.c_str(), "r");
  if (bio == nullptr) return Status::not_found("cannot open certificate: " + pem_path);
  X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (cert == nullptr) return Status::invalid_argument("not a PEM certificate: " + pem_path);
  CertificateInfo info;
  char subject[256] = {0};
  X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject) - 1);
  info.subject = subject;
  const ASN1_TIME* not_after = X509_get0_notAfter(cert);
  struct tm tm {};
  if (ASN1_TIME_to_tm(not_after, &tm) != 1) {
    X509_free(cert);
    return Status::invalid_argument("certificate notAfter unreadable: " + pem_path);
  }
#ifdef _WIN32
  const std::time_t t = _mkgmtime(&tm);
#else
  const std::time_t t = timegm(&tm);
#endif
  info.not_after = std::chrono::system_clock::from_time_t(t);
  X509_free(cert);
  return info;
}

std::vector<std::pair<std::string, std::string>> tls_conf_commands(const TlsConfig& tls) {
  std::vector<std::pair<std::string, std::string>> cmds;
  // These are OpenSSL SSL_CONF commands. Unpatched trantor 1.5.28 applies
  // its own minimum version and cipher list *after* them, which would
  // silently discard MinProtocol and CipherString; the overlay port in
  // cmake/vcpkg-overlay-ports/trantor applies trantor's defaults first so
  // configuration wins. See docs/stage1-report.md.
  cmds.emplace_back("MinProtocol", tls.min_version == "1.3" ? "TLSv1.3" : "TLSv1.2");
  if (!tls.ciphersuites_tls13.empty()) cmds.emplace_back("Ciphersuites", tls.ciphersuites_tls13);
  if (!tls.ciphers_tls12.empty()) cmds.emplace_back("CipherString", tls.ciphers_tls12);
  return cmds;
}

App::App(Config config) : config_(std::move(config)) {}

Status App::configure() {
  auto cert = read_certificate_info(config_.tls.certificate_pem);
  if (!cert.ok()) return cert.status();
  cert_ = cert.value();
  validator_ = std::make_shared<OidcValidator>(config_.oidc);

  auto& app = drogon::app();
  app.setLogLevel(trantor::Logger::kWarn);
  if (config_.io_threads != 0) app.setThreadNum(config_.io_threads);
  app.setIdleConnectionTimeout(60);
  app.setClientMaxBodySize(1024 * 1024);
  // The TLS policy is set globally rather than per listener: unpatched
  // Drogon 1.9.13 drops per-listener SSL commands on Windows (see
  // docs/stage1-report.md), and the server has one policy anyway.
  app.setSSLFiles(config_.tls.certificate_pem, config_.tls.private_key_pem);
  app.setSSLConfigCommands(tls_conf_commands(config_.tls));
  app.addListener(config_.listen_address, config_.listen_port, /*useSSL=*/true);

  auto validator = validator_;
  const CertificateInfo cert_info = cert_;
  app.registerHandler(
      "/healthz",
      [validator, cert_info](drogon::HttpRequestPtr) -> drogon::Task<drogon::HttpResponsePtr> {
        const auto now = std::chrono::system_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::hours>(cert_info.not_after - now);
        const auto snap = validator->snapshot();
        nlohmann::json body;
        body["status"] = "ok";
        body["tls"]["certificate_subject"] = cert_info.subject;
        body["tls"]["not_after_unix"] = std::chrono::system_clock::to_time_t(cert_info.not_after);
        body["tls"]["days_remaining"] = remaining.count() / 24;
        body["tls"]["warn_30_days"] = remaining.count() / 24 <= 30;
        body["tls"]["warn_7_days"] = remaining.count() / 24 <= 7;
        body["oidc"]["jwks_keys"] = snap.key_count;
        body["oidc"]["jwks_fetched_unix"] = std::chrono::system_clock::to_time_t(snap.fetched_at);
        body["oidc"]["jwks_refreshes"] = snap.refreshes;
        body["oidc"]["jwks_refreshes_suppressed"] = snap.refreshes_suppressed;
        co_return json_response(body, drogon::k200OK);
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/whoami",
      [validator](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
        auto who = co_await validator->authenticate(req->getHeader("authorization"));
        if (!who.ok()) {
          nlohmann::json err;
          err["error"] = "unauthorized";
          // The reason is logged, never returned: a caller learns only that
          // the token was rejected.
          LOG_WARN << "whoami rejected: " << who.status().to_string();
          auto resp = json_response(err, drogon::k401Unauthorized);
          resp->addHeader("WWW-Authenticate", "Bearer");
          co_return resp;
        }
        const Principal& p = who.value();
        nlohmann::json body;
        body["oid"] = p.oid;
        body["tid"] = p.tid;
        body["sub"] = p.subject;
        body["preferred_username"] = p.preferred_username;
        body["email"] = p.email;
        body["kid"] = p.key_id;
        co_return json_response(body, drogon::k200OK);
      },
      {drogon::Get});
  return Status();
}

drogon::Task<Status> App::initialize_auth() { co_return co_await validator_->initialize(); }

Status App::run(std::function<void()> ready) {
  auto& app = drogon::app();
  init_status_ = Status();
  app.getLoop()->queueInLoop([this, ready]() {
    // Runs on the app loop once it is live. Initialise auth before anything
    // is served; if it fails, stop.
    [](App* self, std::function<void()> ready_cb) -> drogon::AsyncTask {
      // Listeners are bound asynchronously on the IO loops, and the issuer
      // may be a local test issuer on this very process, so a connection
      // failure in the first seconds is retried. Any other failure (bad
      // certificate, wrong issuer, malformed keys) is final.
      Status s;
      for (int attempt = 0; attempt < 50; ++attempt) {
        co_await drogon::sleepCoro(drogon::app().getLoop(), std::chrono::milliseconds(attempt == 0 ? 50 : 100));
        s = co_await self->initialize_auth();
        if (s.ok()) break;
        const bool connection_class = s.message().find("Bad server address") != std::string::npos ||
                                      s.message().find("Network failure") != std::string::npos;
        if (!connection_class) break;
      }
      if (!s.ok()) {
        self->init_status_ = s;
        LOG_ERROR << "OIDC initialisation failed: " << s.to_string();
        drogon::app().quit();
        co_return;
      }
      if (ready_cb) ready_cb();
    }(this, ready);
  });
  app.run();
  return init_status_;
}

}  // namespace archivum::server
