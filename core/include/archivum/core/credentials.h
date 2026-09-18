// Secrets and their verifiers. Argon2id through libsodium (rulings v3:
// libsodium is retained for Argon2). A verifier is opaque bytes; the
// secret itself is never stored.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "archivum/engine/types.h"
#include "archivum/status.h"

namespace archivum::core {

// Cryptographically random secret, base64url without padding.
std::string random_secret(std::size_t bytes = 32);
engine::UuidBytes random_uuid();
std::string uuid_to_string(const engine::UuidBytes& u);
Result<engine::UuidBytes> uuid_from_string(const std::string& s);

// Argon2id verifier for `secret` at the interactive cost (fast enough for
// a per-request device check, slow enough for an offline guess).
Result<std::vector<std::byte>> make_verifier(const std::string& secret);
// Constant-time verification.
bool verify_secret(const std::vector<std::byte>& verifier, const std::string& secret);

}  // namespace archivum::core
