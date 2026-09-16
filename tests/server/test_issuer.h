// A local OIDC test issuer: serves a discovery document and a JWKS on the
// process's Drogon app, and mints Entra-shaped RS256 tokens. Test only; a
// production build refuses to be configured against it.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "test_pki.h"

namespace archivum::testing {

class TestIssuer {
 public:
  // `issuer` is the exact `iss` value and the base URL of the listener that
  // serves it, e.g. https://127.0.0.1:20443
  explicit TestIssuer(std::string issuer);

  // Registers /.well-known/openid-configuration and /jwks on drogon::app().
  // Must be called before app().run().
  void register_routes();

  // Publishes a signing key under `kid`. Keys can be added after the app
  // is running, to simulate rotation.
  void add_key(const std::string& kid, pki::RsaKey key);
  void remove_key(const std::string& kid);
  void set_jwks_max_age(std::uint32_t seconds);

  struct Claims {
    std::string kid = "k1";
    std::string issuer;    // empty: this issuer
    std::string audience = "api://archivum";
    std::string subject = "sub-0001";
    std::string oid = "8f1c2b2e-0000-4000-8000-000000000001";
    std::string tid = "7a3d5c11-0000-4000-8000-0000000000aa";
    std::string preferred_username = "employee@example.test";
    std::string email = "employee@example.test";
    int expires_in_seconds = 3600;
    int not_before_offset_seconds = -60;
    bool omit_oid = false;
    bool omit_exp = false;
  };
  // Signs with the key registered under claims.kid (the key need not be
  // published in the JWKS, which is how "unknown kid" is produced).
  std::string mint(const Claims& claims) const;

  std::uint64_t jwks_requests() const;
  const std::string& issuer() const { return issuer_; }

 private:
  std::string issuer_;
  mutable std::mutex mu_;
  std::map<std::string, pki::RsaKey> keys_;
  std::map<std::string, pki::RsaKey> unpublished_;
  std::uint32_t max_age_ = 3600;
  std::uint64_t jwks_requests_ = 0;
};

}  // namespace archivum::testing
