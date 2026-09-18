// Identifier allocation for tables with a single integer key: the next id
// is one more than the largest present, read inside the caller's write
// transaction, so two writers cannot hand out the same id (there is one
// writer at a time).
#pragma once

#include <cstdint>
#include <string>

#include "archivum/engine/store.h"

namespace archivum::core {

Result<std::int64_t> next_id(engine::Reader& reader, const std::string& table);

}  // namespace archivum::core
