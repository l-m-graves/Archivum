// Randomness for the engine: WAL salts and database ids. libsodium's
// randombytes_buf, which is thread-safe, keeps no state of ours (invariant
// I1), and cannot fail once sodium_init() has succeeded. std::random_device
// can throw when the entropy source is unavailable, and a throw inside a
// checkpoint is a worse failure than the shared-state race it replaced
// (Stage 6 ruling). So: initialise once at process start and fail startup;
// Db::open calls init_entropy() too, so a caller that skipped it fails at
// open, never inside a checkpoint.
#pragma once

#include <cstddef>
#include <span>

#include "archivum/status.h"

namespace archivum {

// Idempotent and thread-safe. Io when the library cannot initialise.
Status init_entropy();

// Fills `out` with random bytes. Precondition: init_entropy() returned ok
// (it aborts the process otherwise, by libsodium's contract; that is the
// point of failing at startup).
void random_bytes(std::span<std::byte> out);

}  // namespace archivum
