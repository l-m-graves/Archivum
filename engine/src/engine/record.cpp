#include "archivum/engine/record.h"

#include <cstring>
#include <array>
#include <cstddef>

#include "archivum/engine/page_format.h"

namespace archivum::engine {
namespace {

void put_be64_flipped(std::int64_t v, Bytes& out) {
  const std::uint64_t u = static_cast<std::uint64_t>(v) ^ 0x8000000000000000ull;
  for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::byte>((u >> (8 * i)) & 0xFFu));
}
std::int64_t get_be64_flipped(const std::byte* p) {
  std::uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u = (u << 8) | std::to_integer<std::uint64_t>(p[i]);
  return static_cast<std::int64_t>(u ^ 0x8000000000000000ull);
}

void put_escaped(std::span<const std::byte> bytes, Bytes& out) {
  for (std::byte b : bytes) {
    out.push_back(b);
    if (b == std::byte{0}) out.push_back(std::byte{0xFF});
  }
  out.push_back(std::byte{0});
  out.push_back(std::byte{0});
}

Status get_escaped(std::span<const std::byte> in, std::size_t& pos, Bytes& out) {
  out.clear();
  while (true) {
    if (pos >= in.size()) return Status::corrupt("unterminated key string");
    const std::byte b = in[pos++];
    if (b != std::byte{0}) {
      out.push_back(b);
      continue;
    }
    if (pos >= in.size()) return Status::corrupt("unterminated key string");
    const std::byte n = in[pos++];
    if (n == std::byte{0}) return Status();
    if (n == std::byte{0xFF}) {
      out.push_back(std::byte{0});
      continue;
    }
    return Status::corrupt("bad key string escape");
  }
}

}  // namespace

void encode_key_value(const Value& v, Bytes& out) {
  if (v.is_null()) {
    out.push_back(std::byte{0});
    return;
  }
  out.push_back(std::byte{1});
  switch (v.kind()) {
    case Value::Kind::Integer:
    case Value::Kind::Decimal:
    case Value::Kind::Timestamp:
      put_be64_flipped(v.as_int64(), out);
      break;
    case Value::Kind::Boolean:
      out.push_back(static_cast<std::byte>(v.as_bool() ? 1 : 0));
      break;
    case Value::Kind::Uuid:
      out.insert(out.end(), v.as_uuid().begin(), v.as_uuid().end());
      break;
    case Value::Kind::Text: {
      const auto& t = v.as_text();
      put_escaped(std::span<const std::byte>(reinterpret_cast<const std::byte*>(t.data()), t.size()), out);
      break;
    }
    case Value::Kind::Blob:
      put_escaped(v.as_blob(), out);
      break;
    case Value::Kind::Null:
      break;
  }
}

Bytes encode_key(const Row& values) {
  Bytes out;
  for (const auto& v : values) encode_key_value(v, out);
  return out;
}

Status decode_key(std::span<const std::byte> in, std::size_t& pos, const std::vector<ColumnType>& types,
                  Row& out) {
  out.clear();
  for (ColumnType t : types) {
    if (pos >= in.size()) return Status::corrupt("key too short");
    const std::byte tag = in[pos++];
    if (tag == std::byte{0}) {
      out.push_back(Value::null());
      continue;
    }
    if (tag != std::byte{1}) return Status::corrupt("bad key tag");
    switch (t) {
      case ColumnType::Integer:
      case ColumnType::Decimal:
      case ColumnType::Timestamp: {
        if (pos + 8 > in.size()) return Status::corrupt("key integer too short");
        const std::int64_t i = get_be64_flipped(in.data() + pos);
        pos += 8;
        out.push_back(t == ColumnType::Integer ? Value::integer(i)
                                               : (t == ColumnType::Decimal ? Value::decimal(i) : Value::timestamp(i)));
        break;
      }
      case ColumnType::Boolean:
        if (pos >= in.size()) return Status::corrupt("key boolean too short");
        out.push_back(Value::boolean(in[pos++] != std::byte{0}));
        break;
      case ColumnType::Uuid: {
        if (pos + 16 > in.size()) return Status::corrupt("key uuid too short");
        UuidBytes u;
        std::memcpy(u.data(), in.data() + pos, 16);
        pos += 16;
        out.push_back(Value::uuid(u));
        break;
      }
      case ColumnType::Text:
      case ColumnType::Blob: {
        Bytes raw;
        if (Status s = get_escaped(in, pos, raw); !s.ok()) return s;
        if (t == ColumnType::Text) {
          out.push_back(Value::text(std::string(reinterpret_cast<const char*>(raw.data()), raw.size())));
        } else {
          out.push_back(Value::blob(std::move(raw)));
        }
        break;
      }
    }
  }
  return Status();
}

Bytes encode_row(const Row& row) {
  Bytes out;
  out.push_back(static_cast<std::byte>(row.size() & 0xFF));
  out.push_back(static_cast<std::byte>((row.size() >> 8) & 0xFF));
  for (const auto& v : row) {
    out.push_back(static_cast<std::byte>(v.kind()));
    switch (v.kind()) {
      case Value::Kind::Null:
        break;
      case Value::Kind::Integer:
      case Value::Kind::Decimal:
      case Value::Kind::Timestamp: {
        std::array<std::byte, 8> b{};
        put_u64(b.data(), static_cast<std::uint64_t>(v.as_int64()));
        out.insert(out.end(), b.begin(), b.end());
        break;
      }
      case Value::Kind::Boolean:
        out.push_back(static_cast<std::byte>(v.as_bool() ? 1 : 0));
        break;
      case Value::Kind::Uuid:
        out.insert(out.end(), v.as_uuid().begin(), v.as_uuid().end());
        break;
      case Value::Kind::Text:
      case Value::Kind::Blob: {
        std::span<const std::byte> bytes =
            v.kind() == Value::Kind::Text
                ? std::span<const std::byte>(reinterpret_cast<const std::byte*>(v.as_text().data()), v.as_text().size())
                : std::span<const std::byte>(v.as_blob());
        std::array<std::byte, 4> len{};
        put_u32(len.data(), static_cast<std::uint32_t>(bytes.size()));
        out.insert(out.end(), len.begin(), len.end());
        out.insert(out.end(), bytes.begin(), bytes.end());
        break;
      }
    }
  }
  return out;
}

Result<Row> decode_row(std::span<const std::byte> in) {
  if (in.size() < 2) return Status::corrupt("row too short");
  const std::size_t n = std::to_integer<std::size_t>(in[0]) | (std::to_integer<std::size_t>(in[1]) << 8);
  std::size_t pos = 2;
  Row row;
  row.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (pos >= in.size()) return Status::corrupt("row truncated");
    const auto kind = static_cast<Value::Kind>(std::to_integer<std::uint8_t>(in[pos++]));
    switch (kind) {
      case Value::Kind::Null:
        row.push_back(Value::null());
        break;
      case Value::Kind::Integer:
      case Value::Kind::Decimal:
      case Value::Kind::Timestamp: {
        if (pos + 8 > in.size()) return Status::corrupt("row integer truncated");
        const auto i64 = static_cast<std::int64_t>(get_u64(in.data() + pos));
        pos += 8;
        row.push_back(kind == Value::Kind::Integer ? Value::integer(i64)
                                                   : (kind == Value::Kind::Decimal ? Value::decimal(i64) : Value::timestamp(i64)));
        break;
      }
      case Value::Kind::Boolean:
        if (pos >= in.size()) return Status::corrupt("row boolean truncated");
        row.push_back(Value::boolean(in[pos++] != std::byte{0}));
        break;
      case Value::Kind::Uuid: {
        if (pos + 16 > in.size()) return Status::corrupt("row uuid truncated");
        UuidBytes u;
        std::memcpy(u.data(), in.data() + pos, 16);
        pos += 16;
        row.push_back(Value::uuid(u));
        break;
      }
      case Value::Kind::Text:
      case Value::Kind::Blob: {
        if (pos + 4 > in.size()) return Status::corrupt("row length truncated");
        const std::uint32_t len = get_u32(in.data() + pos);
        pos += 4;
        if (pos + len > in.size()) return Status::corrupt("row payload truncated");
        if (kind == Value::Kind::Text) {
          row.push_back(Value::text(std::string(reinterpret_cast<const char*>(in.data() + pos), len)));
        } else {
          row.push_back(Value::blob(Bytes(in.begin() + static_cast<std::ptrdiff_t>(pos),
                                          in.begin() + static_cast<std::ptrdiff_t>(pos + len))));
        }
        pos += len;
        break;
      }
      default:
        return Status::corrupt("row has unknown value kind");
    }
  }
  if (pos != in.size()) return Status::corrupt("row has trailing bytes");
  return row;
}

}  // namespace archivum::engine
