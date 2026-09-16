#include "test_issuer.h"

#include <drogon/drogon.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <nlohmann/json.hpp>

namespace archivum::testing {

using traits = jwt::traits::nlohmann_json;

TestIssuer::TestIssuer(std::string issuer) : issuer_(std::move(issuer)) {}

void TestIssuer::register_routes() {
  auto& app = drogon::app();
  app.registerHandler(
      "/.well-known/openid-configuration",
      [this](const drogon::HttpRequestPtr&,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        nlohmann::json doc;
        doc["issuer"] = issuer_;
        doc["jwks_uri"] = issuer_ + "/jwks";
        doc["token_endpoint"] = issuer_ + "/oauth2/v2.0/token";
        doc["id_token_signing_alg_values_supported"] = {"RS256"};
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(doc.dump());
        callback(resp);
      },
      {drogon::Get});
  app.registerHandler(
      "/jwks",
      [this](const drogon::HttpRequestPtr&,
             std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        nlohmann::json doc;
        doc["keys"] = nlohmann::json::array();
        std::uint32_t max_age = 0;
        {
          std::lock_guard<std::mutex> lock(mu_);
          ++jwks_requests_;
          max_age = max_age_;
          for (const auto& [kid, key] : keys_) {
            nlohmann::json jwk;
            jwk["kty"] = "RSA";
            jwk["use"] = "sig";
            jwk["alg"] = "RS256";
            jwk["kid"] = kid;
            jwk["n"] = key.n_b64url;
            jwk["e"] = key.e_b64url;
            doc["keys"].push_back(jwk);
          }
        }
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->addHeader("Cache-Control", "max-age=" + std::to_string(max_age));
        resp->setBody(doc.dump());
        callback(resp);
      },
      {drogon::Get});
}

void TestIssuer::add_key(const std::string& kid, pki::RsaKey key) {
  std::lock_guard<std::mutex> lock(mu_);
  keys_[kid] = key;
  unpublished_.erase(kid);
}

void TestIssuer::remove_key(const std::string& kid) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = keys_.find(kid);
  if (it == keys_.end()) return;
  unpublished_[kid] = it->second;  // still usable for minting, no longer published
  keys_.erase(it);
}

void TestIssuer::set_jwks_max_age(std::uint32_t seconds) {
  std::lock_guard<std::mutex> lock(mu_);
  max_age_ = seconds;
}

std::string TestIssuer::mint(const Claims& c) const {
  pki::RsaKey key;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = keys_.find(c.kid);
    if (it != keys_.end()) {
      key = it->second;
    } else {
      auto u = unpublished_.find(c.kid);
      if (u == unpublished_.end()) throw std::runtime_error("no key for kid " + c.kid);
      key = u->second;
    }
  }
  const auto now = std::chrono::system_clock::now();
  auto builder = jwt::create<traits>()
                     .set_type("JWT")
                     .set_algorithm("RS256")
                     .set_key_id(c.kid)
                     .set_issuer(c.issuer.empty() ? issuer_ : c.issuer)
                     .set_audience(c.audience)
                     .set_subject(c.subject)
                     .set_issued_at(now)
                     .set_not_before(now + std::chrono::seconds(c.not_before_offset_seconds))
                     .set_payload_claim("tid", jwt::basic_claim<traits>(c.tid))
                     .set_payload_claim("preferred_username", jwt::basic_claim<traits>(c.preferred_username))
                     .set_payload_claim("email", jwt::basic_claim<traits>(c.email))
                     .set_payload_claim("ver", jwt::basic_claim<traits>(std::string("2.0")));
  if (!c.omit_oid) builder.set_payload_claim("oid", jwt::basic_claim<traits>(c.oid));
  if (!c.omit_exp) builder.set_expires_at(now + std::chrono::seconds(c.expires_in_seconds));
  return builder.sign(jwt::algorithm::rs256(key.public_pem, key.private_pem, "", ""));
}

std::uint64_t TestIssuer::jwks_requests() const {
  std::lock_guard<std::mutex> lock(mu_);
  return jwks_requests_;
}

}  // namespace archivum::testing
