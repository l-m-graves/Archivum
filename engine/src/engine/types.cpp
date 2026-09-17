#include "archivum/engine/types.h"

#include <cstring>

namespace archivum::engine {

const char* to_string(ColumnType t) {
  switch (t) {
    case ColumnType::Integer:
      return "integer";
    case ColumnType::Decimal:
      return "decimal";
    case ColumnType::Text:
      return "text";
    case ColumnType::Blob:
      return "blob";
    case ColumnType::Timestamp:
      return "timestamp";
    case ColumnType::Boolean:
      return "boolean";
    case ColumnType::Uuid:
      return "uuid";
  }
  return "?";
}

Value Value::text(std::string s) {
  Value v(Kind::Text, 0);
  v.text_ = std::move(s);
  return v;
}
Value Value::blob(std::vector<std::byte> b) {
  Value v(Kind::Blob, 0);
  v.blob_ = std::move(b);
  return v;
}
Value Value::uuid(const UuidBytes& u) {
  Value v(Kind::Uuid, 0);
  v.uuid_ = u;
  return v;
}

bool Value::matches(ColumnType t) const {
  switch (kind_) {
    case Kind::Null:
      return true;
    case Kind::Integer:
      return t == ColumnType::Integer;
    case Kind::Decimal:
      return t == ColumnType::Decimal;
    case Kind::Text:
      return t == ColumnType::Text;
    case Kind::Blob:
      return t == ColumnType::Blob;
    case Kind::Timestamp:
      return t == ColumnType::Timestamp;
    case Kind::Boolean:
      return t == ColumnType::Boolean;
    case Kind::Uuid:
      return t == ColumnType::Uuid;
  }
  return false;
}

int Value::compare(const Value& a, const Value& b) {
  if (a.kind_ == Kind::Null || b.kind_ == Kind::Null) {
    if (a.kind_ == b.kind_) return 0;
    return a.kind_ == Kind::Null ? -1 : 1;
  }
  if (a.kind_ != b.kind_) return static_cast<int>(a.kind_) < static_cast<int>(b.kind_) ? -1 : 1;
  switch (a.kind_) {
    case Kind::Integer:
    case Kind::Decimal:
    case Kind::Timestamp:
    case Kind::Boolean:
      return a.i_ < b.i_ ? -1 : (a.i_ > b.i_ ? 1 : 0);
    case Kind::Text: {
      const int c = a.text_.compare(b.text_);
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    case Kind::Blob: {
      const std::size_t n = std::min(a.blob_.size(), b.blob_.size());
      const int c = n == 0 ? 0 : std::memcmp(a.blob_.data(), b.blob_.data(), n);
      if (c != 0) return c < 0 ? -1 : 1;
      return a.blob_.size() < b.blob_.size() ? -1 : (a.blob_.size() > b.blob_.size() ? 1 : 0);
    }
    case Kind::Uuid: {
      const int c = std::memcmp(a.uuid_.data(), b.uuid_.data(), 16);
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    case Kind::Null:
      break;
  }
  return 0;
}

std::string Value::to_string() const {
  switch (kind_) {
    case Kind::Null:
      return "NULL";
    case Kind::Integer:
    case Kind::Decimal:
    case Kind::Timestamp:
      return std::to_string(i_);
    case Kind::Boolean:
      return i_ ? "true" : "false";
    case Kind::Text:
      return "'" + text_ + "'";
    case Kind::Blob:
      return "blob(" + std::to_string(blob_.size()) + ")";
    case Kind::Uuid: {
      static const char* hex = "0123456789abcdef";
      std::string s;
      for (std::size_t i = 0; i < 16; ++i) {
        const auto b = std::to_integer<unsigned>(uuid_[i]);
        s += hex[b >> 4];
        s += hex[b & 15];
        if (i == 3 || i == 5 || i == 7 || i == 9) s += '-';
      }
      return s;
    }
  }
  return "?";
}

bool valid_utf8(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 0;
    std::uint32_t cp = 0;
    if (c < 0x80) {
      len = 1;
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + len > s.size()) return false;
    for (std::size_t k = 1; k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

}  // namespace archivum::engine
