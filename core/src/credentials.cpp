#include "archivum/core/credentials.h"

#include <sodium.h>

#include <cstring>

#include "archivum/entropy.h"

namespace archivum::core {
namespace {

// The process initialised libsodium at startup (archivum::init_entropy,
// which every Db::open also calls); this is the idempotent re-check, so a
// unit test that never opened a database still gets a working library.
void ensure_sodium() { (void)init_entropy(); }

const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

}  // namespace

std::string random_secret(std::size_t bytes) {
  ensure_sodium();
  std::vector<unsigned char> raw(bytes);
  randombytes_buf(raw.data(), raw.size());
  std::string out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (unsigned char b : raw) {
    acc = (acc << 8) | b;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += kAlphabet[(acc >> bits) & 63];
    }
  }
  if (bits > 0) out += kAlphabet[(acc << (6 - bits)) & 63];
  return out;
}

engine::UuidBytes random_uuid() {
  ensure_sodium();
  engine::UuidBytes u{};
  randombytes_buf(u.data(), u.size());
  u[6] = static_cast<std::byte>((std::to_integer<unsigned>(u[6]) & 0x0F) | 0x40);  // version 4
  u[8] = static_cast<std::byte>((std::to_integer<unsigned>(u[8]) & 0x3F) | 0x80);  // variant
  return u;
}

std::string uuid_to_string(const engine::UuidBytes& u) { return engine::Value::uuid(u).to_string(); }

Result<engine::UuidBytes> uuid_from_string(const std::string& s) {
  engine::UuidBytes u{};
  std::size_t n = 0;
  int hi = -1;
  for (char c : s) {
    if (c == '-') continue;
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return Status::invalid_argument("not a uuid");
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= 16) return Status::invalid_argument("not a uuid");
      u[n++] = static_cast<std::byte>((hi << 4) | v);
      hi = -1;
    }
  }
  if (n != 16 || hi >= 0) return Status::invalid_argument("not a uuid");
  return u;
}

Result<std::vector<std::byte>> make_verifier(const std::string& secret) {
  ensure_sodium();
  char out[crypto_pwhash_STRBYTES];
  if (crypto_pwhash_str(out, secret.data(), secret.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                        crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
    return Status::io("argon2id: out of memory");
  }
  const std::size_t len = std::strlen(out);
  std::vector<std::byte> v(len);
  std::memcpy(v.data(), out, len);
  return v;
}

bool verify_secret(const std::vector<std::byte>& verifier, const std::string& secret) {
  ensure_sodium();
  if (verifier.empty() || verifier.size() >= crypto_pwhash_STRBYTES) return false;
  char buf[crypto_pwhash_STRBYTES] = {0};
  std::memcpy(buf, verifier.data(), verifier.size());
  return crypto_pwhash_str_verify(buf, secret.data(), secret.size()) == 0;
}

}  // namespace archivum::core
