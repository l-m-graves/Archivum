#include "archivum/server/oidc.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <drogon/HttpClient.h>
#include <jwt-cpp/traits/nlohmann-json/traits.h>
#include <nlohmann/json.hpp>

namespace archivum::server {
namespace {

using traits = jwt::traits::nlohmann_json;

struct SplitUrl {
  std::string base;  // https://host:port
  std::string path;  // /path?query
};

bool split_url(const std::string& url, SplitUrl& out) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return false;
  const auto path_start = url.find('/', scheme + 3);
  if (path_start == std::string::npos) {
    out.base = url;
    out.path = "/";
  } else {
    out.base = url.substr(0, path_start);
    out.path = url.substr(path_start);
  }
  return true;
}

// Parses "max-age=N" out of a Cache-Control value. Returns 0 if absent.
std::uint32_t parse_max_age(const std::string& cache_control) {
  std::string lower(cache_control);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto pos = lower.find("max-age=");
  if (pos == std::string::npos) return 0;
  const long v = std::strtol(lower.c_str() + pos + 8, nullptr, 10);
  return v > 0 ? static_cast<std::uint32_t>(v) : 0;
}

std::string claim_string(const jwt::decoded_jwt<traits>& jwt, const char* name) {
  if (!jwt.has_payload_claim(name)) return "";
  const auto claim = jwt.get_payload_claim(name);
  if (claim.get_type() != jwt::json::type::string) return "";
  return claim.as_string();
}

}  // namespace

OidcValidator::OidcValidator(OidcConfig config) : config_(std::move(config)) {}

drogon::Task<Result<OidcValidator::Fetched>> OidcValidator::https_get(std::string url) {
  SplitUrl parts;
  if (!split_url(url, parts) || parts.base.rfind("https://", 0) != 0) {
    co_return Status::invalid_argument("OIDC URL must be https://: " + url);
  }
  // validateCert = true: chain, expiry, and hostname are all checked. The
  // optional CA bundle adds trust anchors for a non-production issuer; the
  // OS store is always consulted too.
  auto client = drogon::HttpClient::newHttpClient(parts.base, nullptr, /*useOldTLS=*/false,
                                                  /*validateCert=*/true);
  if (!config_.ca_bundle_pem.empty()) {
    client->addSSLConfigs({{"VerifyCAFile", config_.ca_bundle_pem}});
  }
  client->setUserAgent("archivum");
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Get);
  req->setPath(parts.path);
  try {
    auto resp = co_await client->sendRequestCoro(req, /*timeout=*/10.0);
    if (resp->statusCode() != drogon::k200OK) {
      co_return Status::io("OIDC fetch of " + url + " returned HTTP " +
                           std::to_string(static_cast<int>(resp->statusCode())));
    }
    Fetched f;
    f.body = std::string(resp->body());
    f.cache_control = resp->getHeader("cache-control");
    co_return f;
  } catch (const drogon::HttpException& e) {
    co_return Status::io("OIDC fetch of " + url + " failed: " + e.what());
  } catch (const std::exception& e) {
    co_return Status::io("OIDC fetch of " + url + " failed: " + e.what());
  }
}

drogon::Task<Status> OidcValidator::initialize() {
  auto disc = co_await https_get(config_.discovery_url);
  if (!disc.ok()) co_return disc.status();
  nlohmann::json doc = nlohmann::json::parse(disc.value().body, nullptr, false);
  if (!doc.is_object() || !doc.contains("issuer") || !doc.contains("jwks_uri")) {
    co_return Status::io("discovery document lacks issuer or jwks_uri");
  }
  if (doc["issuer"].get<std::string>() != config_.issuer) {
    co_return Status::io("discovery issuer '" + doc["issuer"].get<std::string>() +
                         "' does not match configured issuer '" + config_.issuer + "'");
  }
  const std::string jwks_uri = doc["jwks_uri"].get<std::string>();
  if (jwks_uri.rfind("https://", 0) != 0) co_return Status::io("jwks_uri is not https://");
  {
    std::lock_guard<std::mutex> lock(mu_);
    jwks_uri_ = jwks_uri;
  }
  co_return co_await refresh_jwks();
}

drogon::Task<Status> OidcValidator::refresh_jwks() {
  std::string uri;
  {
    std::lock_guard<std::mutex> lock(mu_);
    uri = jwks_uri_;
    last_refresh_started_ = std::chrono::steady_clock::now();
  }
  auto fetched = co_await https_get(uri);
  if (!fetched.ok()) co_return fetched.status();

  std::map<std::string, std::string> keys;
  try {
    const auto jwks = jwt::parse_jwks<traits>(fetched.value().body);
    for (const auto& jwk : jwks) {
      if (!jwk.has_key_id() || jwk.get_key_type() != "RSA") continue;
      if (jwk.has_use() && jwk.get_use() != "sig") continue;
      const std::string n = jwk.get_jwk_claim("n").as_string();
      const std::string e = jwk.get_jwk_claim("e").as_string();
      keys[jwk.get_key_id()] = jwt::helper::create_public_key_from_rsa_components(n, e);
    }
  } catch (const std::exception& e) {
    co_return Status::io(std::string("JWKS parse failed: ") + e.what());
  }
  if (keys.empty()) co_return Status::io("JWKS contains no usable RSA signing keys");

  std::uint32_t max_age = parse_max_age(fetched.value().cache_control);
  if (max_age == 0) max_age = config_.jwks_default_max_age_seconds;
  {
    std::lock_guard<std::mutex> lock(mu_);
    keys_ = std::move(keys);
    fetched_at_ = std::chrono::system_clock::now();
    expires_at_ = fetched_at_ + std::chrono::seconds(max_age);
    ++refreshes_;
  }
  co_return Status();
}

Result<Principal> OidcValidator::verify_with_cached_keys(const std::string& token,
                                                        bool& unknown_kid) const {
  unknown_kid = false;
  jwt::decoded_jwt<traits> decoded = jwt::decode<traits>(token);
  if (!decoded.has_key_id()) return Status::invalid_argument("token has no kid");
  const std::string kid = decoded.get_key_id();
  if (decoded.get_algorithm() != "RS256") {
    return Status::invalid_argument("token algorithm is not RS256");
  }
  std::string pem;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = keys_.find(kid);
    if (it == keys_.end()) {
      unknown_kid = true;
      return Status::not_found("unknown kid");
    }
    pem = it->second;
  }
  auto verifier = jwt::verify<traits>()
                      .allow_algorithm(jwt::algorithm::rs256(pem, "", "", ""))
                      .with_issuer(config_.issuer)
                      .with_audience(config_.audience)
                      .leeway(config_.clock_skew_seconds);
  std::error_code ec;
  verifier.verify(decoded, ec);
  if (ec) return Status::invalid_argument("token rejected: " + ec.message());
  // `exp` is mandatory: jwt-cpp only checks it when present.
  if (!decoded.has_expires_at()) return Status::invalid_argument("token has no exp");

  Principal p;
  p.subject = claim_string(decoded, "sub");
  p.oid = claim_string(decoded, "oid");
  p.tid = claim_string(decoded, "tid");
  p.preferred_username = claim_string(decoded, "preferred_username");
  p.email = claim_string(decoded, "email");
  p.key_id = kid;
  if (p.oid.empty() || p.tid.empty()) {
    return Status::invalid_argument("token lacks oid or tid");
  }
  return p;
}

drogon::Task<Result<Principal>> OidcValidator::authenticate(std::string authorization_header) {
  // "Bearer " prefix, case-insensitive scheme, exactly one token.
  if (authorization_header.size() < 8) co_return Status::invalid_argument("missing bearer token");
  std::string scheme = authorization_header.substr(0, 7);
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (scheme != "bearer ") co_return Status::invalid_argument("authorization scheme is not Bearer");
  std::string token = authorization_header.substr(7);
  while (!token.empty() && token.front() == ' ') token.erase(token.begin());
  if (token.empty() || token.find(' ') != std::string::npos) {
    co_return Status::invalid_argument("malformed bearer token");
  }

  // Refresh first if the cache has expired per the endpoint's max-age.
  bool expired = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    expired = std::chrono::system_clock::now() >= expires_at_;
  }
  if (expired) {
    if (Status s = co_await refresh_single_flight(/*floor=*/false); !s.ok()) co_return s;
  }

  bool unknown_kid = false;
  Result<Principal> first = [&]() -> Result<Principal> {
    try {
      return verify_with_cached_keys(token, unknown_kid);
    } catch (const std::exception& e) {
      return Status::invalid_argument(std::string("token undecodable: ") + e.what());
    }
  }();
  if (first.ok() || !unknown_kid) co_return first;

  // Unknown kid: one refresh, shared by every request that hits it at the
  // same time, and rate-limited so a flood of bad kids cannot hammer the
  // issuer.
  if (Status s = co_await refresh_single_flight(/*floor=*/true); !s.ok()) co_return s;
  try {
    co_return verify_with_cached_keys(token, unknown_kid);
  } catch (const std::exception& e) {
    co_return Status::invalid_argument(std::string("token undecodable: ") + e.what());
  }
}

drogon::Task<Status> OidcValidator::refresh_single_flight(bool floor) {
  enum class Role { Fetch, Wait, Refuse };
  Role role = Role::Fetch;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    generation = refresh_generation_;
    if (refresh_in_flight_) {
      ++refreshes_joined_;
      role = Role::Wait;
    } else if (floor && std::chrono::steady_clock::now() - last_refresh_started_ <
                            std::chrono::seconds(config_.jwks_refresh_min_interval_seconds)) {
      ++refreshes_suppressed_;
      role = Role::Refuse;
    } else {
      refresh_in_flight_ = true;
    }
  }
  if (role == Role::Refuse) co_return Status::invalid_argument("unknown kid; refresh suppressed by rate floor");
  if (role == Role::Fetch) {
    Status s = co_await refresh_jwks();
    std::lock_guard<std::mutex> lock(mu_);
    refresh_in_flight_ = false;
    ++refresh_generation_;
    co_return s;
  }
  // A refresh is in flight on another request: wait for it to finish
  // (bounded), then the caller re-verifies against the refreshed keys.
  for (int i = 0; i < 2000; ++i) {
    co_await drogon::sleepCoro(trantor::EventLoop::getEventLoopOfCurrentThread(), std::chrono::milliseconds(5));
    std::lock_guard<std::mutex> lock(mu_);
    if (refresh_generation_ != generation) co_return Status();
  }
  co_return Status::io("JWKS refresh in flight did not finish in time");
}

OidcValidator::Snapshot OidcValidator::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  Snapshot s;
  s.jwks_uri = jwks_uri_;
  s.key_count = keys_.size();
  s.fetched_at = fetched_at_;
  s.expires_at = expires_at_;
  s.refreshes = refreshes_;
  s.refreshes_suppressed = refreshes_suppressed_;
  s.refreshes_joined = refreshes_joined_;
  return s;
}

void OidcValidator::expire_cache_for_test() {
  std::lock_guard<std::mutex> lock(mu_);
  expires_at_ = std::chrono::system_clock::time_point{};
  last_refresh_started_ = std::chrono::steady_clock::time_point{};
}

}  // namespace archivum::server
