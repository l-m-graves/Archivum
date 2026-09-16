#include "archivum/server/config.h"

#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace archivum::server {
namespace {

using nlohmann::json;

// Allow-list check: every key in `obj` must be in `allowed`. Unknown keys
// are errors, never ignored (a misspelled "tls" must not silently disable it).
Status expect_keys(const json& obj, const char* where, const std::set<std::string>& allowed) {
  if (!obj.is_object()) return Status::invalid_argument(std::string(where) + " must be an object");
  for (const auto& [key, value] : obj.items()) {
    if (allowed.count(key) == 0) {
      return Status::invalid_argument(std::string("unknown key '") + key + "' in " + where);
    }
  }
  return Status();
}

template <class T>
Status get_opt(const json& obj, const char* key, const char* where, T& out) {
  if (!obj.contains(key)) return Status();
  try {
    out = obj.at(key).get<T>();
  } catch (const json::exception& e) {
    return Status::invalid_argument(std::string(where) + "." + key + ": " + e.what());
  }
  return Status();
}

Status get_req(const json& obj, const char* key, const char* where, std::string& out) {
  if (!obj.contains(key)) {
    return Status::invalid_argument(std::string(where) + "." + key + " is required");
  }
  if (Status s = get_opt(obj, key, where, out); !s.ok()) return s;
  if (out.empty()) return Status::invalid_argument(std::string(where) + "." + key + " is empty");
  return Status();
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

std::string host_of(const std::string& url) {
  const auto scheme = url.find("://");
  if (scheme == std::string::npos) return "";
  const auto start = scheme + 3;
  const auto end = url.find_first_of("/:?", start);
  return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// A test issuer lives on a loopback or private address. Production builds
// refuse it (instructions v2, Q1).
bool is_local_or_private_host(const std::string& host) {
  if (host == "localhost" || host == "::1" || starts_with(host, "127.") || starts_with(host, "10.") ||
      starts_with(host, "192.168.") || starts_with(host, "169.254.")) {
    return true;
  }
  if (starts_with(host, "172.")) {
    const auto second = host.find('.', 4);
    if (second != std::string::npos) {
      const int octet = std::atoi(host.substr(4, second - 4).c_str());
      if (octet >= 16 && octet <= 31) return true;
    }
  }
  return host.find('.') == std::string::npos;  // a bare hostname with no domain
}

bool file_readable(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return in.good();
}

}  // namespace

Result<Config> parse_config(const std::string& json_text) {
  json root;
  try {
    root = json::parse(json_text);
  } catch (const json::exception& e) {
    return Status::invalid_argument(std::string("config is not valid JSON: ") + e.what());
  }
  Config c;
  if (Status s = expect_keys(root, "config", {"listen", "tls", "oidc", "trusted_proxies"}); !s.ok()) {
    return s;
  }

  if (root.contains("listen")) {
    const json& l = root["listen"];
    if (Status s = expect_keys(l, "listen", {"address", "port", "io_threads"}); !s.ok()) return s;
    if (Status s = get_opt(l, "address", "listen", c.listen_address); !s.ok()) return s;
    if (Status s = get_opt(l, "port", "listen", c.listen_port); !s.ok()) return s;
    if (Status s = get_opt(l, "io_threads", "listen", c.io_threads); !s.ok()) return s;
    if (c.listen_port == 0) return Status::invalid_argument("listen.port must be non-zero");
  }

  if (!root.contains("tls")) return Status::invalid_argument("tls section is required; there is no plaintext mode");
  {
    const json& t = root["tls"];
    if (Status s = expect_keys(t, "tls", {"certificate_pem", "private_key_pem", "min_version",
                                          "ciphers_tls12", "ciphersuites_tls13"});
        !s.ok()) {
      return s;
    }
    if (Status s = get_req(t, "certificate_pem", "tls", c.tls.certificate_pem); !s.ok()) return s;
    if (Status s = get_req(t, "private_key_pem", "tls", c.tls.private_key_pem); !s.ok()) return s;
    if (Status s = get_opt(t, "min_version", "tls", c.tls.min_version); !s.ok()) return s;
    if (Status s = get_opt(t, "ciphers_tls12", "tls", c.tls.ciphers_tls12); !s.ok()) return s;
    if (Status s = get_opt(t, "ciphersuites_tls13", "tls", c.tls.ciphersuites_tls13); !s.ok()) return s;
    if (c.tls.min_version != "1.2" && c.tls.min_version != "1.3") {
      return Status::invalid_argument("tls.min_version must be \"1.2\" or \"1.3\"");
    }
    if (!file_readable(c.tls.certificate_pem)) {
      return Status::invalid_argument("tls.certificate_pem is not readable: " + c.tls.certificate_pem);
    }
    if (!file_readable(c.tls.private_key_pem)) {
      return Status::invalid_argument("tls.private_key_pem is not readable: " + c.tls.private_key_pem);
    }
  }

  if (!root.contains("oidc")) return Status::invalid_argument("oidc section is required");
  {
    const json& o = root["oidc"];
    if (Status s = expect_keys(o, "oidc",
                               {"issuer", "audience", "discovery_url", "ca_bundle_pem",
                                "clock_skew_seconds", "jwks_refresh_min_interval_seconds",
                                "jwks_default_max_age_seconds"});
        !s.ok()) {
      return s;
    }
    if (Status s = get_req(o, "issuer", "oidc", c.oidc.issuer); !s.ok()) return s;
    if (Status s = get_req(o, "audience", "oidc", c.oidc.audience); !s.ok()) return s;
    if (Status s = get_req(o, "discovery_url", "oidc", c.oidc.discovery_url); !s.ok()) return s;
    if (Status s = get_opt(o, "ca_bundle_pem", "oidc", c.oidc.ca_bundle_pem); !s.ok()) return s;
    if (Status s = get_opt(o, "clock_skew_seconds", "oidc", c.oidc.clock_skew_seconds); !s.ok()) return s;
    if (Status s = get_opt(o, "jwks_refresh_min_interval_seconds", "oidc",
                           c.oidc.jwks_refresh_min_interval_seconds);
        !s.ok()) {
      return s;
    }
    if (Status s = get_opt(o, "jwks_default_max_age_seconds", "oidc", c.oidc.jwks_default_max_age_seconds);
        !s.ok()) {
      return s;
    }
    if (!starts_with(c.oidc.discovery_url, "https://")) {
      return Status::invalid_argument("oidc.discovery_url must be https://");
    }
    if (!starts_with(c.oidc.issuer, "https://")) {
      return Status::invalid_argument("oidc.issuer must be https://");
    }
    if (c.oidc.clock_skew_seconds > 300) {
      return Status::invalid_argument("oidc.clock_skew_seconds must be at most 300");
    }
    if (!c.oidc.ca_bundle_pem.empty() && !file_readable(c.oidc.ca_bundle_pem)) {
      return Status::invalid_argument("oidc.ca_bundle_pem is not readable: " + c.oidc.ca_bundle_pem);
    }
#if ARCHIVUM_PRODUCTION_BUILD
    // A production build trusts only the operating system's store and only
    // an issuer on a public host. Neither is configurable.
    if (!c.oidc.ca_bundle_pem.empty()) {
      return Status::invalid_argument(
          "oidc.ca_bundle_pem is refused in a production build; the OS trust store is used");
    }
    if (is_local_or_private_host(host_of(c.oidc.discovery_url)) ||
        is_local_or_private_host(host_of(c.oidc.issuer))) {
      return Status::invalid_argument("test issuer refused in a production build");
    }
#else
    (void)is_local_or_private_host;
    (void)host_of;
#endif
  }

  if (root.contains("trusted_proxies")) {
    if (Status s = get_opt(root, "trusted_proxies", "config", c.trusted_proxies); !s.ok()) return s;
  }
  return c;
}

Result<Config> load_config(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Status::not_found("config file not readable: " + path);
  std::ostringstream text;
  text << in.rdbuf();
  return parse_config(text.str());
}

}  // namespace archivum::server
