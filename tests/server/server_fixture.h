// Shared fixture for server tests: one Drogon app per process, hosting the
// test issuer (good and bad-certificate listeners) and the server under
// test, all on 127.0.0.1 with ports derived from the process id.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <thread>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include "archivum/core/authz.h"
#include "archivum/core/recorder.h"
#include "archivum/punchline/data.h"
#include "archivum/server/app.h"
#include "test_issuer.h"
#include "test_pki.h"

namespace archivum::testing {

struct Ports {
  std::uint16_t issuer_good;
  std::uint16_t issuer_bad;
  std::uint16_t server;
  std::uint16_t server_tls12;
};

inline Ports pick_ports() {
#ifdef _WIN32
  const unsigned pid = static_cast<unsigned>(::_getpid());
#else
  const unsigned pid = static_cast<unsigned>(::getpid());
#endif
  // Four ports per process, spaced so that two test processes with
  // adjacent pids (ctest -j) never share one.
  const std::uint16_t base = static_cast<std::uint16_t>(20000 + (pid % 10000) * 4);
  return Ports{base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2),
               static_cast<std::uint16_t>(base + 3)};
}

class ServerFixture {
 public:
  static constexpr const char* kTls12Cipher = "ECDHE-RSA-AES128-GCM-SHA256";
  ServerFixture() = default;
  ~ServerFixture() { stop(); }

  // Generates PKI, writes files, configures the app, and runs it on a
  // background thread until OIDC initialisation completes.
  archivum::Status start(const std::string& min_tls_version = "1.3",
                         std::uint32_t jwks_refresh_floor_seconds = 1) {
    ports = pick_ports();
    dir = pki::make_temp_dir("archivum-server-test");
    auto ca1 = pki::make_self_signed("archivum-test-ca1", 400);
    if (!ca1.ok()) return ca1.status();
    auto ca2 = pki::make_self_signed("archivum-test-ca2", 400);
    if (!ca2.ok()) return ca2.status();
    ca1_cert = pki::write_file(dir, "ca1.crt", ca1.value().cert_pem);
    ca1_key = pki::write_file(dir, "ca1.key", ca1.value().key_pem);
    ca2_cert = pki::write_file(dir, "ca2.crt", ca2.value().cert_pem);
    ca2_key = pki::write_file(dir, "ca2.key", ca2.value().key_pem);

    issuer = std::make_unique<TestIssuer>("https://127.0.0.1:" + std::to_string(ports.issuer_good));
    auto k1 = pki::generate_rsa();
    if (!k1.ok()) return k1.status();
    issuer->add_key("k1", k1.value());

    archivum::server::Config cfg;
    cfg.listen_address = "127.0.0.1";
    cfg.listen_port = ports.server;
    cfg.io_threads = 2;
    cfg.tls.certificate_pem = ca1_cert;
    cfg.tls.private_key_pem = ca1_key;
    cfg.tls.min_version = min_tls_version;
    cfg.oidc.issuer = issuer->issuer();
    cfg.oidc.audience = "api://archivum";
    cfg.oidc.discovery_url = issuer->issuer() + "/.well-known/openid-configuration";
    cfg.oidc.ca_bundle_pem = ca1_cert;
    cfg.oidc.clock_skew_seconds = 120;
    cfg.oidc.jwks_refresh_min_interval_seconds = jwks_refresh_floor_seconds;
    cfg.oidc.jwks_default_max_age_seconds = 3600;
    // Stage 5: the store, its archive, the off-host destination and the log.
    std::filesystem::create_directories(dir + "/archive");
    std::filesystem::create_directories(dir + "/offhost");
    cfg.database.path = dir + "/live.db";
    cfg.database.archive_dir = dir + "/archive";
    cfg.backup.destination = dir + "/offhost";
    cfg.backup.archive_cadence_seconds = 1;
    cfg.backup.backup_cadence_seconds = 3600;
    cfg.logging.file = dir + "/server.log";
    if (!punchline_config.is_null()) cfg.modules["punchline"] = punchline_config;
    config = cfg;

    app = std::make_unique<archivum::server::App>(cfg);
    if (archivum::Status s = app->configure(); !s.ok()) return s;
    // Seed the default test principal as admin and a mapped employee, so
    // the identity tests have standing; an unmapped principal is 403.
    if (archivum::Status s = seed(); !s.ok()) return s;
    issuer->register_routes();
    auto& d = drogon::app();
    // Good issuer listener: certificate the validator trusts.
    d.addListener("127.0.0.1", ports.issuer_good, true, ca1_cert, ca1_key);
    // Bad issuer listener: certificate the validator must reject.
    d.addListener("127.0.0.1", ports.issuer_bad, true, ca2_cert, ca2_key);
    // A TLS 1.2-minimum listener for protocol policy tests.
    archivum::server::TlsConfig tls12 = cfg.tls;
    tls12.min_version = "1.2";
    tls12.ciphers_tls12 = kTls12Cipher;  // exactly one cipher, so the test can assert it
    d.addListener("127.0.0.1", ports.server_tls12, true, ca1_cert, ca1_key, false,
                  archivum::server::tls_conf_commands(tls12));

    std::promise<void> ready;
    auto ready_future = ready.get_future();
    thread = std::thread([this, &ready]() {
      run_status = app->run([&ready]() { ready.set_value(); });
      if (!run_status.ok()) {
        // Initialisation failed; unblock the waiter so it can read the status.
        try {
          ready.set_value();
        } catch (...) {
        }
      }
      finished = true;
    });
    ready_future.wait();
    client_loop.run();
    return run_status;
  }

  void stop() {
    if (thread.joinable()) {
      if (!finished) drogon::app().quit();
      thread.join();
    }
    // Stop the client loop too, so that no connection is torn down on its
    // thread while OpenSSL's at-exit cleanup runs on the main thread
    // (ThreadSanitizer reported exactly that in CI run 22).
    if (!client_stopped) {
      client_stopped = true;
      client_loop.getLoop()->quit();
      client_loop.wait();
    }
  }

  // /healthz is 503 until the first off-host copy has succeeded; the
  // shipper runs every second in tests. Waits for the first 200.
  bool wait_healthy(int seconds = 15) {
    for (int i = 0; i < seconds * 10; ++i) {
      auto r = get("/healthz");
      if (r.result == drogon::ReqResult::Ok && r.status == 200) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }


  struct Response {
    drogon::ReqResult result = drogon::ReqResult::Ok;
    int status = 0;
    std::string body;
    std::string www_authenticate;
  };

  // Synchronous request against the server under test, verifying its
  // certificate against ca1.
  Response get(const std::string& path, const std::string& authorization = "",
               std::uint16_t port = 0) {
    auto client = drogon::HttpClient::newHttpClient(
        "https://127.0.0.1:" + std::to_string(port == 0 ? ports.server : port), client_loop.getLoop(),
        false, true);
    client->addSSLConfigs({{"VerifyCAFile", ca1_cert}});
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath(path);
    if (!authorization.empty()) req->addHeader("Authorization", authorization);
    auto [result, resp] = client->sendRequest(req, 10.0);
    Response r;
    r.result = result;
    if (result == drogon::ReqResult::Ok && resp) {
      r.status = static_cast<int>(resp->statusCode());
      r.body = std::string(resp->body());
      r.www_authenticate = resp->getHeader("www-authenticate");
    }
    return r;
  }

  static constexpr const char* kAdminOid = "8f1c2b2e-0000-4000-8000-000000000001";
  static constexpr const char* kEmployeeOid = "8f1c2b2e-0000-4000-8000-000000000002";
  static constexpr const char* kSupervisorOid = "8f1c2b2e-0000-4000-8000-000000000003";
  static constexpr const char* kPayrollOid = "8f1c2b2e-0000-4000-8000-000000000004";
  // Set before start(): the `modules.punchline` section (thresholds, cadences).
  nlohmann::json punchline_config;
  static constexpr const char* kTid = "7a3d5c11-0000-4000-8000-0000000000aa";

  archivum::Status seed() {
    auto& store = app->store();
    auto w = store.begin_write();
    if (!w.ok()) return w.status();
    archivum::core::Actor sys;
    sys.account = "test-seed";
    archivum::core::Recorder rec(*w.value(), app->policy(), sys, "test.seed", store.db().now_us());
    if (!rec.status().ok()) return rec.status();
    if (auto r = archivum::core::grant_role(rec, kTid, kAdminOid, "admin", "test-seed", 1); !r.ok()) return r.status();
    archivum::punchline::Employee e;
    e.id = 1;
    e.employee_number = "E0001";
    e.display_name = "Mapped Employee";
    e.tid = kTid;
    e.oid = kEmployeeOid;
    e.site_zone = "America/Los_Angeles";
    e.created_at = e.updated_at = 1;
    if (archivum::Status s = rec.insert("employees", e.to_row()); !s.ok()) return s;
    return w.value()->commit();
  }

  // Synchronous request with a JSON body.
  Response send(drogon::HttpMethod method, const std::string& path, const std::string& body,
                const std::string& authorization = "", std::uint16_t port = 0) {
    auto client = drogon::HttpClient::newHttpClient(
        "https://127.0.0.1:" + std::to_string(port == 0 ? ports.server : port), client_loop.getLoop(), false, true);
    client->addSSLConfigs({{"VerifyCAFile", ca1_cert}});
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(path);
    if (!body.empty()) {
      req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
      req->setBody(body);
    }
    if (!authorization.empty()) req->addHeader("Authorization", authorization);
    auto [result, resp] = client->sendRequest(req, 10.0);
    Response r;
    r.result = result;
    if (result == drogon::ReqResult::Ok && resp) {
      r.status = static_cast<int>(resp->statusCode());
      r.body = std::string(resp->body());
      r.www_authenticate = resp->getHeader("www-authenticate");
    }
    return r;
  }
  Response post(const std::string& path, const std::string& body, const std::string& authorization = "") {
    return send(drogon::Post, path, body, authorization);
  }

  std::string log_text() const {
    std::ifstream in(dir + "/server.log");
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  Ports ports{};
  std::string dir, ca1_cert, ca1_key, ca2_cert, ca2_key;
  std::unique_ptr<TestIssuer> issuer;
  std::unique_ptr<archivum::server::App> app;
  archivum::server::Config config;
  std::thread thread;
  std::atomic<bool> finished{false};
  bool client_stopped = false;
  archivum::Status run_status;
  trantor::EventLoopThread client_loop{"archivum-test-client"};
};

}  // namespace archivum::testing
