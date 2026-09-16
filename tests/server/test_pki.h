// Test-only PKI: RSA keys, self-signed certificates, temp files. Everything
// is generated at run time so no key material lives in the repository.
#pragma once

#include <string>
#include <vector>

#include "archivum/status.h"

namespace archivum::testing::pki {

struct RsaKey {
  std::string private_pem;
  std::string public_pem;
  std::string n_b64url;  // modulus, base64url, for a JWK
  std::string e_b64url;  // exponent, base64url
};
Result<RsaKey> generate_rsa(int bits = 2048);

struct SelfSigned {
  std::string cert_pem;
  std::string key_pem;
};
// CN=`cn`, SAN DNS:localhost and IP:127.0.0.1, CA:TRUE so the certificate
// can serve as its own trust anchor, valid from one hour ago for `days`.
Result<SelfSigned> make_self_signed(const std::string& cn, int days);

std::string make_temp_dir(const std::string& prefix);
std::string write_file(const std::string& dir, const std::string& name, const std::string& content);
std::string base64url(const std::vector<unsigned char>& bytes);

}  // namespace archivum::testing::pki
