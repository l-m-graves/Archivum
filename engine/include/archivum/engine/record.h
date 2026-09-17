// Encodings of typed values for storage.
//
// Key encoding is order-preserving under memcmp so that the b-tree stays
// type-agnostic: a composite key is the concatenation of its components,
// each tagged (0x00 for NULL, which sorts first; 0x01 otherwise), integers as
// sign-flipped big-endian, text and blobs with 0x00 escaped as 0x00 0xFF and
// terminated by 0x00 0x00, booleans as one byte, UUIDs raw.
//
// Row encoding is compact and not ordered: a column count, then per column a
// kind byte and a fixed or length-prefixed payload.
#pragma once

#include <span>
#include <vector>

#include "archivum/engine/types.h"

namespace archivum::engine {

using Bytes = std::vector<std::byte>;

// Appends the order-preserving encoding of `v` to `out`.
void encode_key_value(const Value& v, Bytes& out);
Bytes encode_key(const Row& values);
// Decodes `count` components of the given types from `in`; advances `pos`.
Status decode_key(std::span<const std::byte> in, std::size_t& pos, const std::vector<ColumnType>& types,
                  Row& out);

Bytes encode_row(const Row& row);
Result<Row> decode_row(std::span<const std::byte> in);

}  // namespace archivum::engine
