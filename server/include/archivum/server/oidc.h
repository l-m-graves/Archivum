// OIDC bearer-token validation.
//
// The server validates tokens; it never implements login. At startup it
// fetches the issuer's discovery document over HTTPS (certificate chain,
// expiry, and hostname verified), reads `jwks_uri`, fetches the key set, and
// caches it per the endpoint's cache headers. A token whose `kid` is not in
// the cache triggers one refresh, rate-limited by a configurable floor.
// Clock skew tolerance is bounded (instructions v2, Q1).
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <drogon/utils/coroutine.h>

#include "archivum/server/config.h"
#include "archivum/status.h"

namespace archivum::server {

struct Principal {
  std::string subject;  // `sub`
  std::string oid;      // Entra object id; the identity key for roles
  std::string tid;      // tenant id; stored beside oid
  std::string preferred_username;
  std::string email;
  std::string key_id;  // `kid` that signed the token
};

class OidcValidator : public std::enable_shared_from_this<OidcValidator> {
 public:
  explicit OidcValidator(OidcConfig config);

  // Fetches discovery and JWKS. The server must not listen until this
  // returns ok.
  drogon::Task<Status> initialize();

  // Validates "Bearer <token>". Refreshes the key set on an unknown `kid`
  // (subject to the rate floor) and on cache expiry.
  drogon::Task<Result<Principal>> authenticate(std::string authorization_header);

  struct Snapshot {
    std::string jwks_uri;
    std::size_t key_count = 0;
    std::chrono::system_clock::time_point fetched_at;
    std::chrono::system_clock::time_point expires_at;
    std::uint64_t refreshes = 0;
    std::uint64_t refreshes_suppressed = 0;  // unknown kid inside the rate floor
  };
  Snapshot snapshot() const;

  // Test hook: force the cache to be considered expired.
  void expire_cache_for_test();

 private:
  struct Fetched {
    std::string body;
    std::string cache_control;
  };
  drogon::Task<Result<Fetched>> https_get(std::string url);
  drogon::Task<Status> refresh_jwks();
  Result<Principal> verify_with_cached_keys(const std::string& token, bool& unknown_kid) const;

  OidcConfig config_;
  mutable std::mutex mu_;
  std::string jwks_uri_;
  std::map<std::string, std::string> keys_;  // kid -> public key PEM
  std::chrono::system_clock::time_point fetched_at_;
  std::chrono::system_clock::time_point expires_at_;
  std::chrono::steady_clock::time_point last_refresh_started_;
  std::uint64_t refreshes_ = 0;
  std::uint64_t refreshes_suppressed_ = 0;
};

}  // namespace archivum::server
