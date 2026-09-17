// Declared column types and the tagged value that carries them.
//
//   Integer    int64
//   Decimal    int64 scaled by the column's declared scale; never floating point
//   Text       UTF-8 bytes (validated as UTF-8 on insert)
//   Blob       bytes
//   Timestamp  int64 microseconds since the Unix epoch, UTC; an instant
//   Boolean
//   UuidBytes       16 bytes
#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <cstdint>
#include <string>
#include <vector>

#include "archivum/status.h"

namespace archivum::engine {

enum class ColumnType : std::uint8_t {
  Integer = 1,
  Decimal = 2,
  Text = 3,
  Blob = 4,
  Timestamp = 5,
  Boolean = 6,
  Uuid = 7,
};

const char* to_string(ColumnType t);

using UuidBytes = std::array<std::byte, 16>;

class Value {
 public:
  enum class Kind : std::uint8_t { Null = 0, Integer, Decimal, Text, Blob, Timestamp, Boolean, Uuid };

  Value() = default;
  static Value null() { return Value(); }
  static Value integer(std::int64_t v) { return Value(Kind::Integer, v); }
  // `units` are already scaled: 12.34 at scale 2 is 1234.
  static Value decimal(std::int64_t units) { return Value(Kind::Decimal, units); }
  static Value timestamp(std::int64_t micros) { return Value(Kind::Timestamp, micros); }
  static Value boolean(bool v) { return Value(Kind::Boolean, v ? 1 : 0); }
  static Value text(std::string s);
  static Value blob(std::vector<std::byte> b);
  static Value uuid(const UuidBytes& u);

  Kind kind() const { return kind_; }
  bool is_null() const { return kind_ == Kind::Null; }
  std::int64_t as_int64() const { return i_; }  // Integer, Decimal, Timestamp
  bool as_bool() const { return i_ != 0; }
  const std::string& as_text() const { return text_; }
  const std::vector<std::byte>& as_blob() const { return blob_; }
  const UuidBytes& as_uuid() const { return uuid_; }

  // True if this value may be stored in a column of type `t` (Null always may).
  bool matches(ColumnType t) const;

  // Three-way comparison of two non-null values of the same kind. Nulls sort
  // before everything; comparing different kinds is a caller error.
  static int compare(const Value& a, const Value& b);
  bool operator==(const Value& o) const { return compare(*this, o) == 0 && kind_ == o.kind_; }
  bool operator!=(const Value& o) const { return !(*this == o); }
  bool operator<(const Value& o) const { return compare(*this, o) < 0; }

  std::string to_string() const;  // for messages and dumps

 private:
  Value(Kind k, std::int64_t i) : kind_(k), i_(i) {}
  Kind kind_ = Kind::Null;
  std::int64_t i_ = 0;
  std::string text_;
  std::vector<std::byte> blob_;
  UuidBytes uuid_{};
};

using Row = std::vector<Value>;

struct ColumnDef {
  std::string name;
  ColumnType type = ColumnType::Integer;
  bool nullable = true;
  std::uint8_t scale = 0;  // Decimal only: digits after the point
};

bool valid_utf8(std::string_view s);

}  // namespace archivum::engine
