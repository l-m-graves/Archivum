#include "archivum/server/app.h"

#include <atomic>
#include <ctime>
#include <set>

#include <drogon/drogon.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "../src/http/json_bridge.h"
#include "../src/http/request.h"
#include "archivum/core/authz.h"
#include "archivum/core/schema.h"
#include "archivum/engine/migrate.h"

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

App::App(Config config, std::vector<ServerModule> modules) : config_(std::move(config)), modules_(std::move(modules)) {}

void App::start_modules() {
  for (const ServerModule& m : modules_) {
    if (m.start) m.start(*this);
  }
}

void App::stop_modules() {
  for (const ServerModule& m : modules_) {
    if (m.stop) m.stop();
  }
}

App::~App() {
  stop_modules();
  if (shipper_) shipper_->stop();
  if (store_) (void)store_->close();
}

Status App::configure() {
  auto logger = Logger::open(config_.logging.file, config_.logging.level);
  if (!logger.ok()) return logger.status();
  set_logger(logger.value());

  auto cert = read_certificate_info(config_.tls.certificate_pem);
  if (!cert.ok()) return cert.status();
  cert_ = cert.value();
  validator_ = std::make_shared<OidcValidator>(config_.oidc);

  // The store: open, migrate every module, build the record policy once.
  vfs_ = make_os_vfs();
  engine::DbOptions dbo;
  dbo.archive_dir = config_.database.archive_dir;
  dbo.now_us = clock_;
  auto st = engine::Store::open(*vfs_, config_.database.path, dbo);
  if (!st.ok()) return st.status();
  store_ = std::move(st.value());
  // Module configuration first: a bad section fails startup before
  // anything is opened or migrated.
  {
    std::set<std::string> known;
    for (const ServerModule& m : modules_) {
      known.insert(m.data->name());
      auto it = config_.modules.find(m.data->name());
      const nlohmann::json section = it == config_.modules.end() ? nlohmann::json::object() : it->second;
      if (m.configure) {
        if (Status s = m.configure(section); !s.ok()) return s;
      } else if (!section.empty()) {
        return Status::invalid_argument("modules." + m.data->name() + " takes no configuration");
      }
    }
    for (const auto& [name, section] : config_.modules) {
      if (known.count(name) == 0) return Status::invalid_argument("modules." + name + ": no such module in this binary");
    }
  }
  std::vector<const core::Module*> data_modules;
  for (const ServerModule& m : modules_) data_modules.push_back(m.data);
  auto schema = core::migrate_all(*store_, data_modules);
  if (!schema.ok()) return schema.status();
  for (const auto& [name, rep] : schema.value().modules) {
    for (const auto& step : rep.applied) log(LogLevel::Info, "schema.migrated", {{"module", name}, {"step", step}});
  }
  policy_ = core::build_policy(data_modules);
  auth_ = std::make_unique<Authenticator>(*store_, validator_, policy_);
  shipper_ = std::make_unique<ArchiveShipper>(*vfs_, *store_, config_.database, config_.backup);

  auto& app = drogon::app();
  app.setLogLevel(trantor::Logger::kWarn);
  if (config_.io_threads != 0) app.setThreadNum(config_.io_threads);
  app.setIdleConnectionTimeout(60);
  app.setClientMaxBodySize(1024 * 1024);
  app.setSSLFiles(config_.tls.certificate_pem, config_.tls.private_key_pem);
  app.setSSLConfigCommands(tls_conf_commands(config_.tls));
  app.addListener(config_.listen_address, config_.listen_port, /*useSSL=*/true);

  App* self = this;
  const CertificateInfo cert_info = cert_;
  app.registerHandler(
      "/healthz",
      [self, cert_info](drogon::HttpRequestPtr) -> drogon::Task<drogon::HttpResponsePtr> {
        const std::string request_id = new_request_id();
        const auto now = std::chrono::system_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::hours>(cert_info.not_after - now);
        const auto snap = self->validator()->snapshot();
        const std::int64_t now_us = self->now_us();
        const ShipStatus ship = self->shipper().status();
        const std::string archive_reason = self->shipper().unhealthy_reason(now_us);
        nlohmann::json body;
        body["tls"]["certificate_subject"] = cert_info.subject;
        body["tls"]["not_after_unix"] = std::chrono::system_clock::to_time_t(cert_info.not_after);
        body["tls"]["days_remaining"] = remaining.count() / 24;
        body["tls"]["warn_30_days"] = remaining.count() / 24 <= 30;
        body["tls"]["warn_7_days"] = remaining.count() / 24 <= 7;
        body["oidc"]["jwks_keys"] = snap.key_count;
        body["oidc"]["jwks_fetched_unix"] = std::chrono::system_clock::to_time_t(snap.fetched_at);
        body["oidc"]["jwks_refreshes"] = snap.refreshes;
        body["oidc"]["jwks_refreshes_suppressed"] = snap.refreshes_suppressed;
        {
          auto rd = self->store().begin_read();
          if (rd.ok()) {
            body["database"]["schema_version"] = rd.value()->catalog().schema_version;
            body["database"]["tables"] = rd.value()->catalog().tables.size();
          }
          const auto stats = self->store().db().stats();
          body["database"]["wal_frames"] = stats.wal_frames;
          body["database"]["checkpoints"] = stats.checkpoints;
          body["database"]["archived_segments"] = stats.archived_segments;
        }
        body["archive"]["destination"] = self->config().backup.destination;
        body["archive"]["last_success_us"] = ship.last_success_us;
        body["archive"]["last_attempt_us"] = ship.last_attempt_us;
        body["archive"]["segments_shipped"] = ship.segments_shipped;
        body["archive"]["backups_shipped"] = ship.backups_shipped;
        body["archive"]["failures"] = ship.failures;
        body["archive"]["verification_failures"] = ship.verification_failures;
        body["archive"]["ok"] = archive_reason.empty();
        if (!archive_reason.empty()) body["archive"]["problem"] = archive_reason;
        body["status"] = archive_reason.empty() ? "ok" : "failing";
        co_return ok_response(body, request_id, archive_reason.empty() ? 200 : 503);
      },
      {drogon::Get});

  app.registerHandler(
      "/api/v1/whoami",
      [self](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
        const std::string request_id = new_request_id();
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) {
          const AuthFailure f = Authenticator::failure_of(who.status());
          log(LogLevel::Warn, "auth.rejected", {{"request_id", request_id}, {"route", "whoami"}, {"reason", f.reason}});
          auto resp = error_response(f.http_status, f.http_status == 403 ? "forbidden" : "unauthorized",
                                     f.http_status == 403 ? "no standing" : "credential rejected", request_id);
          if (f.http_status == 401) resp->addHeader("WWW-Authenticate", "Bearer");
          co_return resp;
        }
        const Caller& c = who.value();
        nlohmann::json body;
        body["kind"] = c.kind == Caller::Kind::Principal ? "principal" : (c.kind == Caller::Kind::Device ? "device" : "account");
        body["oid"] = c.principal.oid;
        body["tid"] = c.principal.tid;
        body["sub"] = c.principal.subject;
        body["preferred_username"] = c.principal.preferred_username;
        body["email"] = c.principal.email;
        body["kid"] = c.principal.key_id;
        body["roles"] = c.roles;
        if (c.employee_id) body["employee_id"] = *c.employee_id;
        if (c.kind == Caller::Kind::Account) body["account"] = c.account;
        co_return ok_response(body, request_id);
      },
      {drogon::Get});

  // Roles as data: grant and revoke, admin only, audited through the recorder.
  app.registerHandler(
      "/api/v1/admin/roles",
      [self](drogon::HttpRequestPtr req) -> drogon::Task<drogon::HttpResponsePtr> {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {"tid", "oid", "role"});
        if (!body.ok()) co_return status_response(body.status(), request_id);
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return error_response(Authenticator::failure_of(who.status()).http_status, "unauthorized", "credential rejected", request_id);
        if (!who.value().has_role("admin")) co_return error_response(403, "forbidden", "admin role required", request_id);
        auto tid = body_string(body.value(), "tid"), oid = body_string(body.value(), "oid"), role = body_string(body.value(), "role");
        for (const auto* r : {&tid, &oid, &role}) {
          if (!r->ok()) co_return status_response(r->status(), request_id);
        }
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "role.grant", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        auto id = core::grant_role(rec, tid.value(), oid.value(), role.value(), who.value().describe(), self->now_us());
        if (!id.ok()) co_return status_response(id.status(), request_id);
        if (Status s = rec.set_target(core::kRoleGrants, std::to_string(id.value())); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        log(LogLevel::Info, "role.granted", {{"request_id", request_id}, {"by", who.value().describe()}, {"role", role.value()}, {"oid", oid.value()}});
        co_return ok_response({{"id", id.value()}}, request_id, 201);
      },
      {drogon::Post});

  app.registerHandler(
      "/api/v1/admin/roles/{id}/revoke",
      [self](drogon::HttpRequestPtr req, std::string id_text) -> drogon::Task<drogon::HttpResponsePtr> {
        const std::string request_id = new_request_id();
        auto body = parse_body(req, {});
        if (!body.ok()) co_return status_response(body.status(), request_id);
        auto who = co_await self->authenticator().authenticate(req, request_id);
        if (!who.ok()) co_return error_response(Authenticator::failure_of(who.status()).http_status, "unauthorized", "credential rejected", request_id);
        if (!who.value().has_role("admin")) co_return error_response(403, "forbidden", "admin role required", request_id);
        char* end = nullptr;
        const long long id = std::strtoll(id_text.c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || id <= 0) co_return error_response(400, "invalid", "bad grant id", request_id);
        auto w = self->store().begin_write();
        if (!w.ok()) co_return status_response(w.status(), request_id);
        core::Recorder rec(*w.value(), self->policy(), who.value().actor(), "role.revoke", self->now_us());
        if (!rec.status().ok()) co_return status_response(rec.status(), request_id);
        if (Status s = core::revoke_role(rec, id); !s.ok()) co_return status_response(s, request_id);
        if (Status s = rec.set_target(core::kRoleGrants, std::to_string(id)); !s.ok()) co_return status_response(s, request_id);
        if (Status s = w.value()->commit(); !s.ok()) co_return status_response(s, request_id);
        co_return ok_response({{"id", id}}, request_id);
      },
      {drogon::Post});

  for (const ServerModule& m : modules_) {
    if (m.register_routes) m.register_routes(*this);
  }
  return Status();
}

drogon::Task<Status> App::initialize_auth() { co_return co_await validator_->initialize(); }

Status App::run(std::function<void()> ready) {
  auto& app = drogon::app();
  init_status_ = Status();
  app.getLoop()->queueInLoop([this, ready]() {
    [](App* self, std::function<void()> ready_cb) -> drogon::AsyncTask {
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
        log(LogLevel::Error, "oidc.init_failed", {{"error", s.to_string()}});
        drogon::app().quit();
        co_return;
      }
      self->shipper().start();
      self->start_modules();
      log(LogLevel::Info, "server.ready", {{"address", self->config().listen_address}, {"port", self->config().listen_port}});
      if (ready_cb) ready_cb();
    }(this, ready);
  });
  app.run();
  stop_modules();
  if (shipper_) shipper_->stop();
  return init_status_;
}

}  // namespace archivum::server
