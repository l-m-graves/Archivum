#include "archivum/entropy.h"

#include <sodium.h>

namespace archivum {

Status init_entropy() {
  // sodium_init() returns 0 on first success, 1 when already initialised,
  // -1 when it cannot initialise. It is safe to call from several threads.
  if (sodium_init() < 0) return Status::io("entropy source unavailable: libsodium failed to initialise");
  return Status();
}

void random_bytes(std::span<std::byte> out) {
  if (out.empty()) return;
  randombytes_buf(out.data(), out.size());
}

}  // namespace archivum
