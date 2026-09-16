#include "test_pki.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace archivum::testing::pki {
namespace {

std::string bio_to_string(BIO* bio) {
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  return len > 0 ? std::string(data, static_cast<std::size_t>(len)) : std::string();
}

std::string bn_b64url(const BIGNUM* bn) {
  std::vector<unsigned char> bytes(static_cast<std::size_t>(BN_num_bytes(bn)));
  BN_bn2bin(bn, bytes.data());
  return base64url(bytes);
}

}  // namespace

std::string base64url(const std::vector<unsigned char>& bytes) {
  static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  std::size_t i = 0;
  while (i + 2 < bytes.size()) {
    const unsigned v = (static_cast<unsigned>(bytes[i]) << 16) |
                       (static_cast<unsigned>(bytes[i + 1]) << 8) | static_cast<unsigned>(bytes[i + 2]);
    out.push_back(alphabet[(v >> 18) & 63]);
    out.push_back(alphabet[(v >> 12) & 63]);
    out.push_back(alphabet[(v >> 6) & 63]);
    out.push_back(alphabet[v & 63]);
    i += 3;
  }
  if (i + 1 == bytes.size()) {
    const unsigned v = static_cast<unsigned>(bytes[i]) << 16;
    out.push_back(alphabet[(v >> 18) & 63]);
    out.push_back(alphabet[(v >> 12) & 63]);
  } else if (i + 2 == bytes.size()) {
    const unsigned v = (static_cast<unsigned>(bytes[i]) << 16) | (static_cast<unsigned>(bytes[i + 1]) << 8);
    out.push_back(alphabet[(v >> 18) & 63]);
    out.push_back(alphabet[(v >> 12) & 63]);
    out.push_back(alphabet[(v >> 6) & 63]);
  }
  return out;
}

Result<RsaKey> generate_rsa(int bits) {
  EVP_PKEY* pkey = EVP_RSA_gen(static_cast<unsigned int>(bits));
  if (pkey == nullptr) return Status::io("EVP_RSA_gen failed");
  RsaKey key;
  BIO* priv = BIO_new(BIO_s_mem());
  BIO* pub = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(priv, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  PEM_write_bio_PUBKEY(pub, pkey);
  key.private_pem = bio_to_string(priv);
  key.public_pem = bio_to_string(pub);
  BIO_free(priv);
  BIO_free(pub);
  BIGNUM* n = nullptr;
  BIGNUM* e = nullptr;
  EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_N, &n);
  EVP_PKEY_get_bn_param(pkey, OSSL_PKEY_PARAM_RSA_E, &e);
  key.n_b64url = bn_b64url(n);
  key.e_b64url = bn_b64url(e);
  BN_free(n);
  BN_free(e);
  EVP_PKEY_free(pkey);
  return key;
}

Result<SelfSigned> make_self_signed(const std::string& cn, int days) {
  EVP_PKEY* pkey = EVP_RSA_gen(2048);
  if (pkey == nullptr) return Status::io("EVP_RSA_gen failed");
  X509* x = X509_new();
  X509_set_version(x, 2);
  std::random_device rd;
  ASN1_INTEGER_set(X509_get_serialNumber(x), static_cast<long>(rd() & 0x7FFFFFFF));
  X509_gmtime_adj(X509_getm_notBefore(x), -3600);
  X509_gmtime_adj(X509_getm_notAfter(x), static_cast<long>(days) * 86400L);
  X509_set_pubkey(x, pkey);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>("Archivum test PKI"), -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
  X509_set_issuer_name(x, name);

  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, x, x, nullptr, nullptr, 0);
  const std::pair<int, const char*> exts[] = {
      {NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1"},
      {NID_basic_constraints, "critical,CA:TRUE"},
      {NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign"},
      {NID_ext_key_usage, "serverAuth"},
  };
  for (const auto& [nid, value] : exts) {
    X509_EXTENSION* ex = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (ex == nullptr) {
      X509_free(x);
      EVP_PKEY_free(pkey);
      return Status::io("X509V3_EXT_conf_nid failed");
    }
    X509_add_ext(x, ex, -1);
    X509_EXTENSION_free(ex);
  }
  if (X509_sign(x, pkey, EVP_sha256()) == 0) {
    X509_free(x);
    EVP_PKEY_free(pkey);
    return Status::io("X509_sign failed");
  }
  SelfSigned out;
  BIO* cert = BIO_new(BIO_s_mem());
  BIO* key = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(cert, x);
  PEM_write_bio_PrivateKey(key, pkey, nullptr, nullptr, 0, nullptr, nullptr);
  out.cert_pem = bio_to_string(cert);
  out.key_pem = bio_to_string(key);
  BIO_free(cert);
  BIO_free(key);
  X509_free(x);
  EVP_PKEY_free(pkey);
  return out;
}

std::string make_temp_dir(const std::string& prefix) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = std::filesystem::temp_directory_path() / (prefix + "-" + std::to_string(stamp));
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::string write_file(const std::string& dir, const std::string& name, const std::string& content) {
  const auto path = (std::filesystem::path(dir) / name).string();
  std::ofstream out(path, std::ios::binary);
  out << content;
  return path;
}

}  // namespace archivum::testing::pki
