// Stage 1 gate, points 1 to 3: TLS server with OpenSSL, drogon::HttpClient
// fetching discovery and JWKS over verified HTTPS, and an Entra-shaped
// token validated end to end against the local test issuer. Also: the
// negative cases that make "validated" mean something.
#include <chrono>
#include <cstring>
#include <thread>

#include <nlohmann/json.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "archivum/server/config.h"
#include "server_fixture.h"
#include "test.h"

using namespace archivum;
using namespace archivum::testing;

namespace {

// Heap-allocated and stopped by the last test rather than at static
// destruction: Drogon's own singletons are destroyed during exit, and
// quitting the app after that point is a use-after-destroy.
ServerFixture& fixture() {
  static ServerFixture* f = nullptr;
  if (f == nullptr) {
    f = new ServerFixture();
    Status s = f->start("1.3", /*jwks_refresh_floor_seconds=*/1);
    if (!s.ok()) std::fprintf(stderr, "fixture start failed: %s\n", s.to_string().c_str());
  }
  return *f;
}

std::string bearer(const std::string& token) { return "Bearer " + token; }

// Raw OpenSSL handshake against a listener, bounded to [min,max] protocol
// versions. Returns the negotiated version string, or empty on failure.
// `cipher_out`, when given, receives the negotiated cipher name.
std::string handshake(std::uint16_t port, int min_version, int max_version, const std::string& ca,
                      std::string* cipher_out = nullptr) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  SSL_CTX_set_min_proto_version(ctx, min_version);
  SSL_CTX_set_max_proto_version(ctx, max_version);
  SSL_CTX_load_verify_locations(ctx, ca.c_str(), nullptr);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
  BIO* bio = BIO_new_ssl_connect(ctx);
  SSL* ssl = nullptr;
  BIO_get_ssl(bio, &ssl);
  SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
  SSL_set1_host(ssl, "localhost");
  const std::string target = "127.0.0.1:" + std::to_string(port);
  BIO_set_conn_hostname(bio, target.c_str());
  std::string negotiated;
  if (BIO_do_connect(bio) > 0 && BIO_do_handshake(bio) > 0) {
    negotiated = SSL_get_version(ssl);
    if (cipher_out != nullptr) *cipher_out = SSL_get_cipher_name(ssl);
    SSL_shutdown(ssl);
  }
  BIO_free_all(bio);
  SSL_CTX_free(ctx);
  return negotiated;
}

}  // namespace

ARCHIVUM_TEST(server_starts_and_fetches_jwks_over_verified_https) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  REQUIRE_OK(f.run_status);
  const auto snap = f.app->validator()->snapshot();
  CHECK(snap.key_count == 1);
  CHECK(snap.jwks_uri == f.issuer->issuer() + "/jwks");
  CHECK(snap.refreshes == 1);
  CHECK(f.issuer->jwks_requests() == 1);
}

ARCHIVUM_TEST(healthz_reports_certificate_and_jwks_state) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  auto r = f.get("/healthz");
  REQUIRE_MSG(r.result == drogon::ReqResult::Ok, "request failed");
  REQUIRE(r.status == 200);
  auto body = nlohmann::json::parse(r.body, nullptr, false);
  REQUIRE(body.is_object());
  CHECK(body["status"] == "ok");
  CHECK(body["tls"]["days_remaining"].get<int>() >= 398);
  CHECK(body["tls"]["warn_30_days"] == false);
  CHECK(body["oidc"]["jwks_keys"] == 1);
}

ARCHIVUM_TEST(whoami_without_token_is_401) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  auto r = f.get("/api/v1/whoami");
  REQUIRE(r.result == drogon::ReqResult::Ok);
  CHECK(r.status == 401);
  CHECK(r.www_authenticate == "Bearer");
  CHECK(f.get("/api/v1/whoami", "Basic abc").status == 401);
  CHECK(f.get("/api/v1/whoami", "Bearer").status == 401);
  CHECK(f.get("/api/v1/whoami", "Bearer not.a.jwt").status == 401);
}

ARCHIVUM_TEST(whoami_with_valid_token_returns_claims) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  TestIssuer::Claims c;
  auto r = f.get("/api/v1/whoami", bearer(f.issuer->mint(c)));
  REQUIRE(r.result == drogon::ReqResult::Ok);
  REQUIRE_MSG(r.status == 200, r.body);
  auto body = nlohmann::json::parse(r.body, nullptr, false);
  REQUIRE(body.is_object());
  CHECK(body["oid"] == c.oid);
  CHECK(body["tid"] == c.tid);
  CHECK(body["sub"] == c.subject);
  CHECK(body["preferred_username"] == c.preferred_username);
  CHECK(body["email"] == c.email);
  CHECK(body["kid"] == "k1");
  // The scheme is case-insensitive per RFC 7235.
  CHECK(f.get("/api/v1/whoami", "bearer " + f.issuer->mint(c)).status == 200);
}

ARCHIVUM_TEST(whoami_rejects_bad_tokens) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  TestIssuer::Claims expired;
  expired.expires_in_seconds = -400;  // beyond the 120 s skew
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(expired))).status == 401);

  TestIssuer::Claims within_skew;
  within_skew.expires_in_seconds = -60;  // inside the 120 s skew: accepted
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(within_skew))).status == 200);

  TestIssuer::Claims future;
  future.not_before_offset_seconds = 600;
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(future))).status == 401);

  TestIssuer::Claims wrong_aud;
  wrong_aud.audience = "api://something-else";
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(wrong_aud))).status == 401);

  TestIssuer::Claims wrong_iss;
  wrong_iss.issuer = "https://login.example.test/other";
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(wrong_iss))).status == 401);

  TestIssuer::Claims no_oid;
  no_oid.omit_oid = true;
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(no_oid))).status == 401);

  TestIssuer::Claims no_exp;
  no_exp.omit_exp = true;
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(no_exp))).status == 401);

  // Tampered payload: flip a character in the middle segment.
  std::string token = f.issuer->mint(TestIssuer::Claims{});
  const auto dot = token.find('.');
  token[dot + 5] = token[dot + 5] == 'A' ? 'B' : 'A';
  CHECK(f.get("/api/v1/whoami", bearer(token)).status == 401);

  // A token signed with a key the issuer never published.
  auto rogue = pki::generate_rsa();
  REQUIRE_OK(rogue.status());
  TestIssuer rogue_issuer(f.issuer->issuer());
  rogue_issuer.add_key("k1", rogue.value());  // same kid, different key
  CHECK(f.get("/api/v1/whoami", bearer(rogue_issuer.mint(TestIssuer::Claims{}))).status == 401);
}

ARCHIVUM_TEST(unknown_kid_triggers_one_rate_limited_refresh) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  auto k2 = pki::generate_rsa();
  REQUIRE_OK(k2.status());
  // Register k2 for minting but keep it out of the JWKS.
  f.issuer->add_key("k2", k2.value());
  f.issuer->remove_key("k2");
  TestIssuer::Claims c;
  c.kid = "k2";

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // clear the 1 s floor
  const auto before = f.app->validator()->snapshot();
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(c))).status == 401);
  auto after = f.app->validator()->snapshot();
  CHECK(after.refreshes == before.refreshes + 1);  // one refetch, still unknown

  // Inside the floor: no second refetch, request still rejected.
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(c))).status == 401);
  after = f.app->validator()->snapshot();
  CHECK(after.refreshes == before.refreshes + 1);
  CHECK(after.refreshes_suppressed >= 1);

  // Rotation: publish k2, wait out the floor, and the token is accepted
  // after exactly one more refresh.
  f.issuer->add_key("k2", k2.value());
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(c))).status == 200);
  after = f.app->validator()->snapshot();
  CHECK(after.refreshes == before.refreshes + 2);
  CHECK(after.key_count == 2);
}

ARCHIVUM_TEST(expired_cache_is_refreshed_per_max_age) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  const auto before = f.app->validator()->snapshot();
  f.app->validator()->expire_cache_for_test();
  CHECK(f.get("/api/v1/whoami", bearer(f.issuer->mint(TestIssuer::Claims{}))).status == 200);
  const auto after = f.app->validator()->snapshot();
  CHECK(after.refreshes == before.refreshes + 1);
  CHECK(after.expires_at > after.fetched_at);
}

ARCHIVUM_TEST(jwks_fetch_fails_against_untrusted_certificate) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  // Same issuer content, served with a certificate the validator does not
  // trust: initialisation must fail. This is the test instructions v2 Q2
  // asks for.
  archivum::server::OidcConfig bad = f.config.oidc;
  const std::string bad_base = "https://127.0.0.1:" + std::to_string(f.ports.issuer_bad);
  bad.discovery_url = bad_base + "/.well-known/openid-configuration";
  auto v = std::make_shared<archivum::server::OidcValidator>(bad);
  Status s = drogon::sync_wait(v->initialize());
  CHECK_MSG(!s.ok(), "fetch against an untrusted certificate must fail");
  CHECK(s.code() == ErrorCode::IoError);

  // And the good issuer with the wrong trust anchor also fails.
  archivum::server::OidcConfig wrong_anchor = f.config.oidc;
  wrong_anchor.ca_bundle_pem = f.ca2_cert;
  auto v2 = std::make_shared<archivum::server::OidcValidator>(wrong_anchor);
  s = drogon::sync_wait(v2->initialize());
  CHECK_MSG(!s.ok(), "fetch with the wrong trust anchor must fail");

  // Wrong issuer string in configuration against a good fetch also fails.
  archivum::server::OidcConfig wrong_issuer = f.config.oidc;
  wrong_issuer.issuer = "https://127.0.0.1:1/other";
  auto v3 = std::make_shared<archivum::server::OidcValidator>(wrong_issuer);
  s = drogon::sync_wait(v3->initialize());
  CHECK(!s.ok());
}

ARCHIVUM_TEST(tls_policy_min_version_is_enforced) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  // The main listener is configured min 1.3.
  CHECK(handshake(f.ports.server, TLS1_3_VERSION, TLS1_3_VERSION, f.ca1_cert) == "TLSv1.3");
  const std::string v12 = handshake(f.ports.server, TLS1_2_VERSION, TLS1_2_VERSION, f.ca1_cert);
  CHECK_MSG(v12.empty(), "TLS 1.2 must be refused by a min-1.3 listener; negotiated '" << v12 << "'");
  CHECK(handshake(f.ports.server, TLS1_1_VERSION, TLS1_1_VERSION, f.ca1_cert).empty());
  // The 1.2-minimum listener accepts 1.2 and 1.3, refuses 1.1, and its
  // configured TLS 1.2 cipher list is what gets negotiated: this is the
  // check that trantor's own cipher list no longer overrides configuration.
  std::string cipher;
  CHECK(handshake(f.ports.server_tls12, TLS1_2_VERSION, TLS1_2_VERSION, f.ca1_cert, &cipher) == "TLSv1.2");
  CHECK_MSG(cipher == ServerFixture::kTls12Cipher, "negotiated cipher '" << cipher << "'");
  CHECK(handshake(f.ports.server_tls12, TLS1_3_VERSION, TLS1_3_VERSION, f.ca1_cert) == "TLSv1.3");
  CHECK(handshake(f.ports.server_tls12, TLS1_1_VERSION, TLS1_1_VERSION, f.ca1_cert).empty());
  // And a client that does not trust the server certificate fails.
  CHECK(handshake(f.ports.server, TLS1_3_VERSION, TLS1_3_VERSION, f.ca2_cert).empty());
}

ARCHIVUM_TEST(config_fails_closed) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  using archivum::server::parse_config;
  auto ok_json = nlohmann::json{
      {"listen", {{"address", "127.0.0.1"}, {"port", 8443}}},
      {"tls", {{"certificate_pem", f.ca1_cert}, {"private_key_pem", f.ca1_key}}},
      {"oidc",
       {{"issuer", "https://login.example.test/tid/v2.0"},
        {"audience", "api://archivum"},
        {"discovery_url", "https://login.example.test/tid/v2.0/.well-known/openid-configuration"}}}};
  REQUIRE_OK(parse_config(ok_json.dump()).status());

  auto no_tls = ok_json;
  no_tls.erase("tls");
  CHECK(!parse_config(no_tls.dump()).ok());

  auto http_disc = ok_json;
  http_disc["oidc"]["discovery_url"] = "http://login.example.test/x";
  CHECK(!parse_config(http_disc.dump()).ok());

  auto unknown_key = ok_json;
  unknown_key["tls"]["allow_insecure"] = true;
  CHECK_MSG(!parse_config(unknown_key.dump()).ok(), "unknown keys must be rejected, not ignored");

  auto bad_version = ok_json;
  bad_version["tls"]["min_version"] = "1.0";
  CHECK(!parse_config(bad_version.dump()).ok());

  auto missing_cert = ok_json;
  missing_cert["tls"]["certificate_pem"] = f.dir + "/does-not-exist.pem";
  CHECK(!parse_config(missing_cert.dump()).ok());

  auto big_skew = ok_json;
  big_skew["oidc"]["clock_skew_seconds"] = 3600;
  CHECK(!parse_config(big_skew.dump()).ok());

  auto local_issuer = ok_json;
  local_issuer["oidc"]["issuer"] = "https://127.0.0.1:20443";
  local_issuer["oidc"]["discovery_url"] = "https://127.0.0.1:20443/.well-known/openid-configuration";
  local_issuer["oidc"]["ca_bundle_pem"] = f.ca1_cert;
#if ARCHIVUM_PRODUCTION_BUILD
  CHECK_MSG(!parse_config(local_issuer.dump()).ok(), "production build must refuse a test issuer");
#else
  CHECK(parse_config(local_issuer.dump()).ok());
#endif
}

// Must be the last test in this file: stops the shared server.
ARCHIVUM_TEST(zz_shutdown_fixture) {
  auto& f = fixture();
  f.stop();
  CHECK(f.finished.load());
}
