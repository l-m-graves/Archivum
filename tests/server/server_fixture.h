// Shared fixture for server tests: one Drogon app per process, hosting the
// test issuer (good and bad-certificate listeners) and the server under
// test, all on 127.0.0.1 with ports derived from the process id.
#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

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
  const std::uint16_t base = static_cast<std::uint16_t>(20000 + (pid % 20000));
  return Ports{base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2),
               static_cast<std::uint16_t>(base + 3)};
}

class ServerFixture {
 public:
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
    config = cfg;

    app = std::make_unique<archivum::server::App>(cfg);
    if (archivum::Status s = app->configure(); !s.ok()) return s;
    issuer->register_routes();
    auto& d = drogon::app();
    // Good issuer listener: certificate the validator trusts.
    d.addListener("127.0.0.1", ports.issuer_good, true, ca1_cert, ca1_key);
    // Bad issuer listener: certificate the validator must reject.
    d.addListener("127.0.0.1", ports.issuer_bad, true, ca2_cert, ca2_key);
    // A TLS 1.2-minimum listener for protocol policy tests.
    archivum::server::TlsConfig tls12 = cfg.tls;
    tls12.min_version = "1.2";
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

  Ports ports{};
  std::string dir, ca1_cert, ca1_key, ca2_cert, ca2_key;
  std::unique_ptr<TestIssuer> issuer;
  std::unique_ptr<archivum::server::App> app;
  archivum::server::Config config;
  std::thread thread;
  std::atomic<bool> finished{false};
  archivum::Status run_status;
  trantor::EventLoopThread client_loop{"archivum-test-client"};
};

}  // namespace archivum::testing
