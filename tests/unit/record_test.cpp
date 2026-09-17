#include "archivum/engine/record.h"

#include <algorithm>
#include <cstring>

#include "test.h"

using namespace archivum;
using namespace archivum::engine;

namespace {
bool key_less(const Row& a, const Row& b) {
  const Bytes ka = encode_key(a), kb = encode_key(b);
  return std::lexicographical_compare(ka.begin(), ka.end(), kb.begin(), kb.end());
}
}  // namespace

ARCHIVUM_TEST(key_encoding_preserves_order) {
  // Integers across the sign boundary.
  const std::int64_t ints[] = {INT64_MIN, -1000000, -1, 0, 1, 42, 1000000, INT64_MAX};
  for (std::size_t i = 0; i + 1 < std::size(ints); ++i) {
    CHECK(key_less({Value::integer(ints[i])}, {Value::integer(ints[i + 1])}));
    CHECK(!key_less({Value::integer(ints[i + 1])}, {Value::integer(ints[i])}));
  }
  // Null sorts first.
  CHECK(key_less({Value::null()}, {Value::integer(INT64_MIN)}));
  // Text: prefixes, embedded NUL, and the byte after NUL.
  CHECK(key_less({Value::text("a")}, {Value::text("ab")}));
  CHECK(key_less({Value::text("ab")}, {Value::text("b")}));
  CHECK(key_less({Value::text("")}, {Value::text("a")}));
  CHECK(key_less({Value::text("a")}, {Value::text(std::string("a\0", 2))}));
  CHECK(key_less({Value::text(std::string("a\0", 2))}, {Value::text(std::string("a\0b", 3))}));
  CHECK(key_less({Value::text(std::string("a\0b", 3))}, {Value::text("a\x01")}));
  // Composite: first component dominates, then the second.
  CHECK(key_less({Value::integer(1), Value::text("zzz")}, {Value::integer(2), Value::text("a")}));
  CHECK(key_less({Value::integer(1), Value::text("a")}, {Value::integer(1), Value::text("b")}));
  CHECK(key_less({Value::integer(1), Value::null()}, {Value::integer(1), Value::text("")}));
  // Booleans and timestamps.
  CHECK(key_less({Value::boolean(false)}, {Value::boolean(true)}));
  CHECK(key_less({Value::timestamp(-5)}, {Value::timestamp(5)}));
}

ARCHIVUM_TEST(key_and_row_round_trip) {
  UuidBytes u{};
  for (std::size_t i = 0; i < 16; ++i) u[i] = static_cast<std::byte>(i * 17);
  Row row = {Value::integer(-7), Value::decimal(123456), Value::text(std::string("he\0llo", 6)),
             Value::blob({std::byte{0}, std::byte{0}, std::byte{0xFF}}), Value::timestamp(1700000000000000),
             Value::boolean(true), Value::uuid(u), Value::null()};
  const std::vector<ColumnType> types = {ColumnType::Integer, ColumnType::Decimal, ColumnType::Text,
                                         ColumnType::Blob, ColumnType::Timestamp, ColumnType::Boolean,
                                         ColumnType::Uuid, ColumnType::Text};
  const Bytes key = encode_key(row);
  Row decoded;
  std::size_t pos = 0;
  REQUIRE_OK(decode_key(key, pos, types, decoded));
  CHECK(pos == key.size());
  REQUIRE(decoded.size() == row.size());
  for (std::size_t i = 0; i < row.size(); ++i) CHECK_MSG(decoded[i] == row[i], "component " << i);

  const Bytes enc = encode_row(row);
  auto back = decode_row(enc);
  REQUIRE_OK(back.status());
  REQUIRE(back.value().size() == row.size());
  for (std::size_t i = 0; i < row.size(); ++i) CHECK_MSG(back.value()[i] == row[i], "column " << i);
  // Truncation is detected.
  Bytes cut(enc.begin(), enc.end() - 3);
  CHECK(decode_row(cut).status().code() == ErrorCode::Corrupt);
  Bytes extra = enc;
  extra.push_back(std::byte{0});
  CHECK(decode_row(extra).status().code() == ErrorCode::Corrupt);
}

ARCHIVUM_TEST(value_semantics) {
  CHECK(Value::integer(1).matches(ColumnType::Integer));
  CHECK(!Value::integer(1).matches(ColumnType::Decimal));
  CHECK(Value::null().matches(ColumnType::Uuid));
  CHECK(Value::compare(Value::null(), Value::integer(0)) < 0);
  CHECK(Value::compare(Value::text("b"), Value::text("a")) > 0);
  CHECK(Value::decimal(100) == Value::decimal(100));
  CHECK(Value::decimal(100) != Value::integer(100));
  CHECK(valid_utf8("héllo"));
  CHECK(!valid_utf8("\xC3"));
  CHECK(!valid_utf8("\xED\xA0\x80"));  // surrogate
  CHECK(valid_utf8(""));
}
