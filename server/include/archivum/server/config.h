// Server configuration. Read once at startup from a JSON file; every field
// is validated and the server refuses to start on any problem. Nothing here
// is a compile-time constant: issuer, audience, JWKS URI, ports, cipher
// list, and trusted proxies are all deployment facts.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "archivum/status.h"

namespace archivum::server {

struct TlsConfig {
  std::string certificate_pem;  // path to the server certificate chain
  std::string private_key_pem;  // path to the private key
  std::string min_version = "1.2";  // "1.2" or "1.3"; there is no lower value
  // OpenSSL cipher list for TLS 1.2 and ciphersuites for TLS 1.3. Empty means
  // the OpenSSL defaults filtered to the configured minimum.
  std::string ciphers_tls12;
  std::string ciphersuites_tls13;
};

struct OidcConfig {
  std::string issuer;         // exact `iss` claim expected
  std::string audience;       // exact `aud` claim expected, e.g. api://archivum
  std::string discovery_url;  // https://.../.well-known/openid-configuration
  std::string ca_bundle_pem;  // PEM bundle used to verify the discovery and JWKS hosts
  std::uint32_t clock_skew_seconds = 120;
  std::uint32_t jwks_refresh_min_interval_seconds = 60;  // floor between unknown-kid refreshes
  std::uint32_t jwks_default_max_age_seconds = 3600;      // when the endpoint sends no cache headers
};

struct DatabaseConfig {
  std::string path;         // the operational database file
  std::string archive_dir;  // log archive directory (docs/backup-recovery.md); required
};

// Off-host copies (instructions v2, Q8). Both are required: a deployment
// with no destination or no cadence does not start, and the health check
// fails until the first successful copy.
struct BackupConfig {
  std::string destination;                     // directory, typically a mounted share or UNC path
  std::uint32_t archive_cadence_seconds = 0;   // how often archived log segments are copied
  std::uint32_t backup_cadence_seconds = 86400;  // how often a full backup is taken and copied
};

struct LoggingConfig {
  std::string level = "info";  // info | warn | error
  std::string file;            // empty: stderr; JSON lines either way
};

struct Config {
  std::string listen_address = "0.0.0.0";
  std::uint16_t listen_port = 8443;
  std::uint32_t io_threads = 0;  // 0: hardware concurrency
  TlsConfig tls;
  OidcConfig oidc;
  DatabaseConfig database;
  BackupConfig backup;
  LoggingConfig logging;
  // Forwarded headers (X-Forwarded-For) are honoured only from these addresses.
  std::vector<std::string> trusted_proxies;
  // Per-module sections, `modules.<name>`, parsed by the module itself with
  // its own allow-list (server/src/modules/*). A section for a module that
  // is not built in is an error.
  std::map<std::string, nlohmann::json> modules;
};

// Parses and validates. Fails closed: a missing certificate, an http://
// discovery URL, an unknown TLS version, or an unreadable CA bundle is an
// error, never a default.
Result<Config> load_config(const std::string& path);
Result<Config> parse_config(const std::string& json_text);

}  // namespace archivum::server
