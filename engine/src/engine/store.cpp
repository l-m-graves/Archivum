#include "archivum/engine/store.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace archivum::engine {
namespace {

constexpr std::byte kVersionKey{0};
constexpr std::byte kTableKeyTag{1};

Bytes table_key(std::string_view name) {
  Bytes k;
  k.push_back(kTableKeyTag);
  for (char c : name) k.push_back(static_cast<std::byte>(c));
  return k;
}

bool starts_with(const Bytes& key, const Bytes& prefix) {
  return key.size() >= prefix.size() && std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}
bool key_less(const Bytes& a, const Bytes& b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

std::vector<ColumnType> types_of(const TableDef& t, const std::vector<std::string>& columns) {
  std::vector<ColumnType> out;
  for (const auto& c : columns) out.push_back(t.columns[static_cast<std::size_t>(t.column_index(c))].type);
  return out;
}

Row project(const TableDef& t, const Row& row, const std::vector<std::string>& columns) {
  Row out;
  out.reserve(columns.size());
  for (const auto& c : columns) out.push_back(row[static_cast<std::size_t>(t.column_index(c))]);
  return out;
}

bool any_null(const Row& r) {
  return std::any_of(r.begin(), r.end(), [](const Value& v) { return v.is_null(); });
}

bool rows_equal(const Row& a, const Row& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

Bytes pk_key_of(const TableDef& t, const Row& row) { return encode_key(project(t, row, t.primary_key)); }

Bytes index_key_of(const TableDef& t, const IndexDef& idx, const Row& row, const Bytes& pk_key) {
  Bytes k = encode_key(project(t, row, idx.columns));
  k.insert(k.end(), pk_key.begin(), pk_key.end());
  return k;
}

bool compare_holds(CheckOp op, const Value& v, const Value& o) {
  switch (op) {
    case CheckOp::Eq:
      return Value::compare(v, o) == 0;
    case CheckOp::Ne:
      return Value::compare(v, o) != 0;
    case CheckOp::Lt:
      return Value::compare(v, o) < 0;
    case CheckOp::Le:
      return Value::compare(v, o) <= 0;
    case CheckOp::Gt:
      return Value::compare(v, o) > 0;
    case CheckOp::Ge:
      return Value::compare(v, o) >= 0;
    case CheckOp::In:
      return false;
  }
  return false;
}

bool check_holds(const TableDef& t, const CheckDef& c, const Row& row) {
  const Value& v = row[static_cast<std::size_t>(t.column_index(c.column))];
  if (v.is_null()) return true;
  if (!c.other_column.empty()) {
    const Value& o = row[static_cast<std::size_t>(t.column_index(c.other_column))];
    if (o.is_null()) return true;
    return compare_holds(c.op, v, o);
  }
  if (c.op == CheckOp::In) {
    return std::any_of(c.operands.begin(), c.operands.end(), [&](const Value& o) { return Value::compare(v, o) == 0; });
  }
  return compare_holds(c.op, v, c.operands[0]);
}

// Type, NOT NULL, UTF-8 and CHECK validation of one row; no lookups.
Status validate_row(const TableDef& t, const Row& row) {
  if (row.size() != t.columns.size()) {
    return Status::invalid_argument("table " + t.name + " has " + std::to_string(t.columns.size()) +
                                    " columns, row has " + std::to_string(row.size()));
  }
  for (std::size_t i = 0; i < row.size(); ++i) {
    const ColumnDef& c = t.columns[i];
    const Value& v = row[i];
    if (v.is_null()) {
      if (!c.nullable) return Status::constraint("not null: " + t.name + "." + c.name);
      continue;
    }
    if (!v.matches(c.type)) {
      return Status::invalid_argument("column " + t.name + "." + c.name + " is " + to_string(c.type) +
                                      ", value " + v.to_string() + " is not");
    }
    if (c.type == ColumnType::Text && !valid_utf8(v.as_text())) {
      return Status::invalid_argument("column " + t.name + "." + c.name + ": text is not valid UTF-8");
    }
  }
  for (const CheckDef& c : t.checks) {
    if (!check_holds(t, c, row)) return Status::constraint("check " + c.name + " on " + t.name);
  }
  return Status();
}

bool pk_supports(const TableDef& t, const std::vector<std::string>& columns) {
  return t.primary_key.size() >= columns.size() && std::equal(columns.begin(), columns.end(), t.primary_key.begin());
}

// The secondary index of `t` whose leading columns are exactly `columns`,
// if any. The primary key may support a lookup too: see pk_supports.
const IndexDef* supporting_index(const TableDef& t, const std::vector<std::string>& columns) {
  for (const IndexDef& idx : t.indexes) {
    if (idx.columns.size() < columns.size()) continue;
    if (std::equal(columns.begin(), columns.end(), idx.columns.begin())) return &idx;
  }
  return nullptr;
}

Status validate_index_def(const TableDef& t, const IndexDef& idx, const IndexDef* self) {
  if (idx.name.empty()) return Status::invalid_argument("index needs a name");
  if (idx.columns.empty()) return Status::invalid_argument("index " + idx.name + " needs columns");
  for (const IndexDef& other : t.indexes) {
    if (&other != self && other.name == idx.name) {
      return Status::already_exists("index " + idx.name + " on " + t.name);
    }
  }
  std::unordered_set<std::string> seen;
  for (const auto& c : idx.columns) {
    if (t.column_index(c) < 0) return Status::invalid_argument("index " + idx.name + ": no column " + c);
    if (!seen.insert(c).second) return Status::invalid_argument("index " + idx.name + ": column " + c + " repeated");
  }
  return Status();
}

Status validate_foreign_key(const Catalog& cat, const TableDef& t, const ForeignKeyDef& fk) {
  if (fk.name.empty()) return Status::invalid_argument("foreign key needs a name");
  if (fk.columns.empty() || fk.columns.size() != fk.ref_columns.size()) {
    return Status::invalid_argument("foreign key " + fk.name + ": column lists differ");
  }
  const TableDef* parent = fk.ref_table == t.name ? &t : cat.table(fk.ref_table);
  if (parent == nullptr) return Status::invalid_argument("foreign key " + fk.name + ": no table " + fk.ref_table);
  bool target_ok = fk.ref_columns == parent->primary_key;
  if (!target_ok) {
    for (const IndexDef& idx : parent->indexes) {
      if (idx.unique && idx.columns == fk.ref_columns) target_ok = true;
    }
  }
  if (!target_ok) {
    return Status::invalid_argument("foreign key " + fk.name + ": referenced columns are not the primary key "
                                    "or a unique index of " + parent->name);
  }
  for (std::size_t i = 0; i < fk.columns.size(); ++i) {
    const int ci = t.column_index(fk.columns[i]);
    const int pi = parent->column_index(fk.ref_columns[i]);
    if (ci < 0) return Status::invalid_argument("foreign key " + fk.name + ": no column " + fk.columns[i]);
    if (pi < 0) return Status::invalid_argument("foreign key " + fk.name + ": no column " + fk.ref_columns[i]);
    const ColumnDef& a = t.columns[static_cast<std::size_t>(ci)];
    const ColumnDef& b = parent->columns[static_cast<std::size_t>(pi)];
    if (a.type != b.type || a.scale != b.scale) {
      return Status::invalid_argument("foreign key " + fk.name + ": " + a.name + " and " + b.name +
                                      " differ in type");
    }
  }
  if (!pk_supports(t, fk.columns) && supporting_index(t, fk.columns) == nullptr) {
    return Status::invalid_argument("foreign key " + fk.name + ": needs an index on " + t.name +
                                    " whose leading columns are the foreign key columns");
  }
  return Status();
}

Status validate_check(const TableDef& t, const CheckDef& c) {
  if (c.name.empty()) return Status::invalid_argument("check needs a name");
  const int ci = t.column_index(c.column);
  if (ci < 0) return Status::invalid_argument("check " + c.name + ": no column " + c.column);
  if (!c.other_column.empty()) {
    // Column to column: same row, same declared type, no constants, no IN.
    const int oi = t.column_index(c.other_column);
    if (oi < 0) return Status::invalid_argument("check " + c.name + ": no column " + c.other_column);
    if (oi == ci) return Status::invalid_argument("check " + c.name + ": compares " + c.column + " with itself");
    if (c.op == CheckOp::In) return Status::invalid_argument("check " + c.name + ": IN takes constants, not a column");
    if (!c.operands.empty()) return Status::invalid_argument("check " + c.name + ": a column comparison takes no constant");
    const ColumnDef& a = t.columns[static_cast<std::size_t>(ci)];
    const ColumnDef& b = t.columns[static_cast<std::size_t>(oi)];
    if (a.type != b.type || a.scale != b.scale) {
      return Status::invalid_argument("check " + c.name + ": " + c.column + " and " + c.other_column + " differ in type");
    }
    return Status();
  }
  if (c.operands.empty()) return Status::invalid_argument("check " + c.name + ": needs an operand");
  if (c.op != CheckOp::In && c.operands.size() != 1) {
    return Status::invalid_argument("check " + c.name + ": needs exactly one operand");
  }
  for (const Value& v : c.operands) {
    if (v.is_null() || !v.matches(t.columns[static_cast<std::size_t>(ci)].type)) {
      return Status::invalid_argument("check " + c.name + ": operand " + v.to_string() + " does not match " +
                                      c.column);
    }
  }
  return Status();
}

Status validate_table_def(const Catalog& cat, const TableDef& t) {
  if (t.name.empty()) return Status::invalid_argument("table needs a name");
  if (t.columns.empty()) return Status::invalid_argument("table " + t.name + " needs columns");
  if (t.columns.size() > 4096) return Status::invalid_argument("table " + t.name + ": too many columns");
  std::unordered_set<std::string> names;
  for (const ColumnDef& c : t.columns) {
    if (c.name.empty()) return Status::invalid_argument("table " + t.name + ": column needs a name");
    if (!names.insert(c.name).second) return Status::invalid_argument("table " + t.name + ": column " + c.name + " repeated");
    if (c.type == ColumnType::Decimal ? c.scale > 18 : c.scale != 0) {
      return Status::invalid_argument("column " + t.name + "." + c.name + ": bad scale");
    }
  }
  if (t.primary_key.empty()) return Status::invalid_argument("table " + t.name + " needs a primary key");
  std::unordered_set<std::string> pk;
  for (const auto& c : t.primary_key) {
    if (t.column_index(c) < 0) return Status::invalid_argument("primary key of " + t.name + ": no column " + c);
    if (!pk.insert(c).second) return Status::invalid_argument("primary key of " + t.name + ": column " + c + " repeated");
  }
  for (const IndexDef& idx : t.indexes) {
    if (Status s = validate_index_def(t, idx, &idx); !s.ok()) return s;
  }
  std::unordered_set<std::string> fk_names;
  for (const ForeignKeyDef& fk : t.foreign_keys) {
    if (!fk_names.insert(fk.name).second) return Status::invalid_argument("foreign key " + fk.name + " repeated");
    if (Status s = validate_foreign_key(cat, t, fk); !s.ok()) return s;
  }
  std::unordered_set<std::string> chk_names;
  for (const CheckDef& c : t.checks) {
    if (!chk_names.insert(c.name).second) return Status::invalid_argument("check " + c.name + " repeated");
    if (Status s = validate_check(t, c); !s.ok()) return s;
  }
  return Status();
}

// Sequential reader over a catalog row.
class RowCursor {
 public:
  explicit RowCursor(const Row& row) : row_(row) {}
  Result<std::string> text() {
    if (pos_ >= row_.size() || row_[pos_].kind() != Value::Kind::Text) return Status::corrupt("catalog: expected text");
    return row_[pos_++].as_text();
  }
  Result<std::int64_t> integer() {
    if (pos_ >= row_.size() || row_[pos_].kind() != Value::Kind::Integer) return Status::corrupt("catalog: expected integer");
    return row_[pos_++].as_int64();
  }
  Result<bool> boolean() {
    if (pos_ >= row_.size() || row_[pos_].kind() != Value::Kind::Boolean) return Status::corrupt("catalog: expected boolean");
    return row_[pos_++].as_bool();
  }
  Result<Value> value() {
    if (pos_ >= row_.size()) return Status::corrupt("catalog: expected value");
    return row_[pos_++];
  }
  Result<std::vector<std::string>> texts() {
    auto n = integer();
    if (!n.ok()) return n.status();
    if (n.value() < 0 || n.value() > 4096) return Status::corrupt("catalog: bad list length");
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < n.value(); ++i) {
      auto s = text();
      if (!s.ok()) return s.status();
      out.push_back(std::move(s.value()));
    }
    return out;
  }
  bool done() const { return pos_ == row_.size(); }

 private:
  const Row& row_;
  std::size_t pos_ = 0;
};

void push_texts(Row& r, const std::vector<std::string>& v) {
  r.push_back(Value::integer(static_cast<std::int64_t>(v.size())));
  for (const auto& s : v) r.push_back(Value::text(s));
}

}  // namespace

// ---- definitions -----------------------------------------------------------

int TableDef::column_index(std::string_view n) const {
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == n) return static_cast<int>(i);
  }
  return -1;
}
const IndexDef* TableDef::index(std::string_view n) const {
  for (const IndexDef& idx : indexes) {
    if (idx.name == n) return &idx;
  }
  return nullptr;
}
const TableDef* Catalog::table(std::string_view n) const {
  auto it = tables.find(std::string(n));
  return it == tables.end() ? nullptr : &it->second;
}

Row encode_table_def(const TableDef& t) {
  Row r;
  r.push_back(Value::text(t.name));
  r.push_back(Value::integer(static_cast<std::int64_t>(t.root)));
  r.push_back(Value::integer(static_cast<std::int64_t>(t.columns.size())));
  for (const ColumnDef& c : t.columns) {
    r.push_back(Value::text(c.name));
    r.push_back(Value::integer(static_cast<std::int64_t>(c.type)));
    r.push_back(Value::boolean(c.nullable));
    r.push_back(Value::integer(c.scale));
  }
  push_texts(r, t.primary_key);
  r.push_back(Value::integer(static_cast<std::int64_t>(t.indexes.size())));
  for (const IndexDef& idx : t.indexes) {
    r.push_back(Value::text(idx.name));
    r.push_back(Value::integer(static_cast<std::int64_t>(idx.root)));
    r.push_back(Value::boolean(idx.unique));
    push_texts(r, idx.columns);
  }
  r.push_back(Value::integer(static_cast<std::int64_t>(t.foreign_keys.size())));
  for (const ForeignKeyDef& fk : t.foreign_keys) {
    r.push_back(Value::text(fk.name));
    r.push_back(Value::text(fk.ref_table));
    push_texts(r, fk.columns);
    push_texts(r, fk.ref_columns);
  }
  r.push_back(Value::integer(static_cast<std::int64_t>(t.checks.size())));
  for (const CheckDef& c : t.checks) {
    r.push_back(Value::text(c.name));
    r.push_back(Value::text(c.column));
    r.push_back(Value::text(c.other_column));  // empty: constant operands follow
    r.push_back(Value::integer(static_cast<std::int64_t>(c.op)));
    r.push_back(Value::integer(static_cast<std::int64_t>(c.operands.size())));
    for (const Value& v : c.operands) r.push_back(v);
  }
  return r;
}

Result<TableDef> decode_table_def(const Row& row) {
  RowCursor c(row);
  TableDef t;
#define ARCHIVUM_TAKE(var, expr)          \
  auto var##_r = (expr);                  \
  if (!var##_r.ok()) return var##_r.status(); \
  auto var = std::move(var##_r.value())
  ARCHIVUM_TAKE(name, c.text());
  t.name = name;
  ARCHIVUM_TAKE(root, c.integer());
  t.root = static_cast<PageNo>(root);
  ARCHIVUM_TAKE(ncols, c.integer());
  if (ncols < 1 || ncols > 4096) return Status::corrupt("catalog: bad column count");
  for (std::int64_t i = 0; i < ncols; ++i) {
    ColumnDef col;
    ARCHIVUM_TAKE(cname, c.text());
    ARCHIVUM_TAKE(ctype, c.integer());
    ARCHIVUM_TAKE(nullable, c.boolean());
    ARCHIVUM_TAKE(scale, c.integer());
    if (ctype < 1 || ctype > 7 || scale < 0 || scale > 18) return Status::corrupt("catalog: bad column");
    col.name = cname;
    col.type = static_cast<ColumnType>(ctype);
    col.nullable = nullable;
    col.scale = static_cast<std::uint8_t>(scale);
    t.columns.push_back(std::move(col));
  }
  ARCHIVUM_TAKE(pk, c.texts());
  t.primary_key = pk;
  ARCHIVUM_TAKE(nidx, c.integer());
  if (nidx < 0 || nidx > 4096) return Status::corrupt("catalog: bad index count");
  for (std::int64_t i = 0; i < nidx; ++i) {
    IndexDef idx;
    ARCHIVUM_TAKE(iname, c.text());
    ARCHIVUM_TAKE(iroot, c.integer());
    ARCHIVUM_TAKE(unique, c.boolean());
    ARCHIVUM_TAKE(icols, c.texts());
    idx.name = iname;
    idx.root = static_cast<PageNo>(iroot);
    idx.unique = unique;
    idx.columns = icols;
    t.indexes.push_back(std::move(idx));
  }
  ARCHIVUM_TAKE(nfk, c.integer());
  if (nfk < 0 || nfk > 4096) return Status::corrupt("catalog: bad foreign key count");
  for (std::int64_t i = 0; i < nfk; ++i) {
    ForeignKeyDef fk;
    ARCHIVUM_TAKE(fname, c.text());
    ARCHIVUM_TAKE(ref, c.text());
    ARCHIVUM_TAKE(cols, c.texts());
    ARCHIVUM_TAKE(refcols, c.texts());
    fk.name = fname;
    fk.ref_table = ref;
    fk.columns = cols;
    fk.ref_columns = refcols;
    t.foreign_keys.push_back(std::move(fk));
  }
  ARCHIVUM_TAKE(nchk, c.integer());
  if (nchk < 0 || nchk > 4096) return Status::corrupt("catalog: bad check count");
  for (std::int64_t i = 0; i < nchk; ++i) {
    CheckDef chk;
    ARCHIVUM_TAKE(kname, c.text());
    ARCHIVUM_TAKE(kcol, c.text());
    ARCHIVUM_TAKE(kother, c.text());
    ARCHIVUM_TAKE(op, c.integer());
    ARCHIVUM_TAKE(nops, c.integer());
    if (op < 1 || op > 7 || nops < 0 || nops > 4096) return Status::corrupt("catalog: bad check");
    if (kother.empty() ? nops < 1 : nops != 0) return Status::corrupt("catalog: bad check operands");
    chk.name = kname;
    chk.column = kcol;
    chk.other_column = kother;
    chk.op = static_cast<CheckOp>(op);
    for (std::int64_t k = 0; k < nops; ++k) {
      ARCHIVUM_TAKE(v, c.value());
      chk.operands.push_back(v);
    }
    t.checks.push_back(std::move(chk));
  }
#undef ARCHIVUM_TAKE
  if (!c.done()) return Status::corrupt("catalog: trailing values");
  return t;
}

Status load_catalog(PageReader& pages, Catalog& out) {
  out = Catalog();
  BTree tree(pages, nullptr, kCatalogRoot);
  return tree.for_each([&](std::span<const std::byte> key, std::span<const std::byte> value) -> Status {
    if (key.empty()) return Status::corrupt("catalog: empty key");
    auto row = decode_row(value);
    if (!row.ok()) return row.status();
    if (key[0] == kVersionKey) {
      if (row.value().size() != 1 || row.value()[0].kind() != Value::Kind::Integer) {
        return Status::corrupt("catalog: bad version entry");
      }
      out.schema_version = static_cast<std::uint64_t>(row.value()[0].as_int64());
      return Status();
    }
    if (key[0] != kTableKeyTag) return Status::corrupt("catalog: unknown entry");
    auto def = decode_table_def(row.value());
    if (!def.ok()) return def.status();
    const std::string name(reinterpret_cast<const char*>(key.data()) + 1, key.size() - 1);
    if (def.value().name != name) return Status::corrupt("catalog: table entry name mismatch");
    out.tables[name] = std::move(def.value());
    return Status();
  });
}

// ---- Reader ----------------------------------------------------------------

Reader::Reader(Store& store, std::shared_ptr<const Catalog> catalog) : store_(store), catalog_(std::move(catalog)) {}
Reader::~Reader() = default;

Result<std::optional<Row>> Reader::get_by_key(const TableDef& t, const Bytes& pk_key) {
  auto v = tree(t.root).get(pk_key);
  if (!v.ok()) return v.status();
  if (!v.value().has_value()) return std::optional<Row>();
  auto row = decode_row(*v.value());
  if (!row.ok()) return row.status();
  return std::optional<Row>(std::move(row.value()));
}

Result<std::optional<Row>> Reader::get(const std::string& table, const Row& primary_key) {
  const TableDef* t = catalog_->table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  if (primary_key.size() != t->primary_key.size()) {
    return Status::invalid_argument("primary key of " + table + " has " + std::to_string(t->primary_key.size()) +
                                    " columns");
  }
  for (std::size_t i = 0; i < primary_key.size(); ++i) {
    if (primary_key[i].is_null() ||
        !primary_key[i].matches(t->columns[static_cast<std::size_t>(t->column_index(t->primary_key[i]))].type)) {
      return Status::invalid_argument("primary key value " + primary_key[i].to_string() + " does not match " +
                                      t->primary_key[i]);
    }
  }
  return get_by_key(*t, encode_key(primary_key));
}

Result<bool> Reader::index_prefix_exists(const TableDef&, const IndexDef& idx, const Row& values) {
  return prefix_exists(idx.root, values);
}

Result<bool> Reader::prefix_exists(PageNo root, const Row& values) {
  const Bytes prefix = encode_key(values);
  BTree tr = tree(root);
  auto c = tr.cursor();
  if (Status s = c.seek(prefix); !s.ok()) return s;
  return c.valid() && starts_with(c.key(), prefix);
}

Status Reader::scan_raw(const TableDef& t, const IndexDef* idx, const std::optional<Bound>& lower,
                        const std::optional<Bound>& upper, bool reverse,
                        const std::function<Result<bool>(const Bytes& key)>& fn) {
  std::vector<std::string> key_columns = idx ? idx->columns : t.primary_key;
  if (idx) key_columns.insert(key_columns.end(), t.primary_key.begin(), t.primary_key.end());
  auto check_bound = [&](const std::optional<Bound>& b) -> Status {
    if (!b) return Status();
    if (b->values.size() > key_columns.size()) return Status::invalid_argument("scan bound has too many columns");
    for (std::size_t i = 0; i < b->values.size(); ++i) {
      const ColumnDef& c = t.columns[static_cast<std::size_t>(t.column_index(key_columns[i]))];
      if (!b->values[i].matches(c.type)) {
        return Status::invalid_argument("scan bound " + b->values[i].to_string() + " does not match " + c.name);
      }
    }
    return Status();
  };
  if (Status s = check_bound(lower); !s.ok()) return s;
  if (Status s = check_bound(upper); !s.ok()) return s;
  const Bytes lo = lower ? encode_key(lower->values) : Bytes();
  const Bytes hi = upper ? encode_key(upper->values) : Bytes();

  BTree tr = tree(idx ? idx->root : t.root);
  auto c = tr.cursor();
  if (!reverse) {
    if (lower) {
      if (Status s = c.seek(lo); !s.ok()) return s;
      if (!lower->inclusive) {
        while (c.valid() && starts_with(c.key(), lo)) {
          if (Status s = c.next(); !s.ok()) return s;
        }
      }
    } else if (Status s = c.seek_first(); !s.ok()) {
      return s;
    }
    while (c.valid()) {
      if (upper) {
        const bool past = upper->inclusive ? (key_less(hi, c.key()) && !starts_with(c.key(), hi))
                                           : !key_less(c.key(), hi);
        if (past) break;
      }
      auto go = fn(c.key());
      if (!go.ok()) return go.status();
      if (!go.value()) break;
      if (Status s = c.next(); !s.ok()) return s;
    }
    return Status();
  }
  if (upper) {
    if (Status s = c.seek(hi); !s.ok()) return s;
    if (upper->inclusive) {
      while (c.valid() && starts_with(c.key(), hi)) {
        if (Status s = c.next(); !s.ok()) return s;
      }
    }
    if (c.valid()) {
      if (Status s = c.prev(); !s.ok()) return s;
    } else if (Status s = c.seek_last(); !s.ok()) {
      return s;
    }
  } else if (Status s = c.seek_last(); !s.ok()) {
    return s;
  }
  while (c.valid()) {
    if (lower) {
      const bool past = lower->inclusive ? key_less(c.key(), lo) : (key_less(c.key(), lo) || starts_with(c.key(), lo));
      if (past) break;
    }
    auto go = fn(c.key());
    if (!go.ok()) return go.status();
    if (!go.value()) break;
    if (Status s = c.prev(); !s.ok()) return s;
  }
  return Status();
}

Status Reader::scan(const std::string& table, const std::string& index, const std::optional<Bound>& lower,
                    const std::optional<Bound>& upper, bool reverse, const std::function<bool(const Row&)>& fn) {
  const TableDef* t = catalog_->table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  const IndexDef* idx = nullptr;
  if (!index.empty()) {
    idx = t->index(index);
    if (idx == nullptr) return Status::not_found("no index " + index + " on " + table);
  }
  const std::vector<ColumnType> idx_types = idx ? types_of(*t, idx->columns) : std::vector<ColumnType>();
  BTree pk_tree = tree(t->root);
  return scan_raw(*t, idx, lower, upper, reverse, [&](const Bytes& key) -> Result<bool> {
    Bytes row_bytes;
    if (idx) {
      Row skip;
      std::size_t pos = 0;
      if (Status s = decode_key(key, pos, idx_types, skip); !s.ok()) return s;
      const Bytes pk_key(key.begin() + static_cast<std::ptrdiff_t>(pos), key.end());
      auto v = pk_tree.get(pk_key);
      if (!v.ok()) return v.status();
      if (!v.value().has_value()) return Status::corrupt("index " + idx->name + " entry without a row");
      row_bytes = std::move(*v.value());
    } else {
      auto v = tree(t->root).get(key);
      if (!v.ok()) return v.status();
      if (!v.value().has_value()) return Status::corrupt("row vanished during scan");
      row_bytes = std::move(*v.value());
    }
    auto row = decode_row(row_bytes);
    if (!row.ok()) return row.status();
    return fn(row.value());
  });
}

Result<std::vector<Row>> Reader::scan_all(const std::string& table, const std::string& index,
                                          const std::optional<Bound>& lower, const std::optional<Bound>& upper,
                                          bool reverse) {
  std::vector<Row> out;
  Status s = scan(table, index, lower, upper, reverse, [&](const Row& r) {
    out.push_back(r);
    return true;
  });
  if (!s.ok()) return s;
  return out;
}

Result<std::uint64_t> Reader::count(const std::string& table) {
  const TableDef* t = catalog_->table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  std::uint64_t n = 0;
  Status s = scan_raw(*t, nullptr, std::nullopt, std::nullopt, false, [&](const Bytes&) -> Result<bool> {
    ++n;
    return true;
  });
  if (!s.ok()) return s;
  return n;
}

Status Reader::check_parents(const TableDef& t, const Row& row, const Row* old_row) {
  for (const ForeignKeyDef& fk : t.foreign_keys) {
    const Row values = project(t, row, fk.columns);
    if (any_null(values)) continue;
    if (old_row && rows_equal(values, project(t, *old_row, fk.columns))) continue;
    const TableDef* parent = fk.ref_table == t.name ? &t : catalog_->table(fk.ref_table);
    if (parent == nullptr) return Status::corrupt("foreign key " + fk.name + " references missing table");
    bool found = false;
    if (fk.ref_columns == parent->primary_key) {
      auto r = get_by_key(*parent, encode_key(values));
      if (!r.ok()) return r.status();
      found = r.value().has_value();
    } else {
      const IndexDef* uidx = nullptr;
      for (const IndexDef& idx : parent->indexes) {
        if (idx.unique && idx.columns == fk.ref_columns) uidx = &idx;
      }
      if (uidx == nullptr) return Status::corrupt("foreign key " + fk.name + " lost its unique index");
      auto r = index_prefix_exists(*parent, *uidx, values);
      if (!r.ok()) return r.status();
      found = r.value();
    }
    if (!found) return Status::constraint("foreign key " + fk.name + " on " + t.name + ": no parent row in " + parent->name);
  }
  return Status();
}

Status Reader::check_no_children(const TableDef& t, const Row& row, const Row* new_row) {
  for (const auto& [cname, child] : catalog_->tables) {
    for (const ForeignKeyDef& fk : child.foreign_keys) {
      if (fk.ref_table != t.name) continue;
      const Row values = project(t, row, fk.ref_columns);
      if (any_null(values)) continue;
      if (new_row && rows_equal(values, project(t, *new_row, fk.ref_columns))) continue;
      Result<bool> r = false;
      if (pk_supports(child, fk.columns)) {
        r = prefix_exists(child.root, values);
      } else {
        const IndexDef* idx = supporting_index(child, fk.columns);
        if (idx == nullptr) return Status::corrupt("foreign key " + fk.name + " lost its index");
        r = prefix_exists(idx->root, values);
      }
      if (!r.ok()) return r.status();
      if (r.value()) {
        return Status::constraint("foreign key " + fk.name + " on " + child.name + ": row of " + t.name +
                                  " is referenced");
      }
    }
  }
  return Status();
}

Status Reader::check_into(StoreCheckReport& rep) {
  auto problem = [&](std::string p) { rep.problems.push_back(std::move(p)); };
  std::unordered_map<PageNo, std::string> owner;
  auto own = [&](const std::vector<PageNo>& pages, const std::string& who) {
    for (PageNo p : pages) {
      auto [it, fresh] = owner.emplace(p, who);
      if (!fresh) problem("page " + std::to_string(p) + " owned by both " + it->second + " and " + who);
    }
  };
  auto check_tree = [&](PageNo root, const std::string& who) -> std::optional<BTreeCheckReport> {
    auto r = tree(root).check();
    if (!r.ok()) {
      problem(who + ": " + r.status().to_string());
      return std::nullopt;
    }
    for (const auto& p : r.value().problems) problem(who + ": " + p);
    own(r.value().pages, who);
    return r.value();
  };
  check_tree(kCatalogRoot, "catalog");
  for (const auto& [name, t] : catalog_->tables) {
    ++rep.tables;
    auto pk_rep = check_tree(t.root, "table " + name);
    std::vector<std::optional<BTreeCheckReport>> idx_reps;
    for (const IndexDef& idx : t.indexes) idx_reps.push_back(check_tree(idx.root, "index " + name + "." + idx.name));
    if (!pk_rep) continue;
    // Definition-level invariants a corrupt catalog could break.
    if (Status s = validate_table_def(*catalog_, t); !s.ok()) problem("table " + name + ": " + s.to_string());
    const std::vector<ColumnType> pk_types = types_of(t, t.primary_key);
    std::uint64_t rows = 0;
    Status s = scan_raw(t, nullptr, std::nullopt, std::nullopt, false, [&](const Bytes& key) -> Result<bool> {
      auto v = tree(t.root).get(key);
      if (!v.ok()) return v.status();
      auto row = decode_row(*v.value());
      if (!row.ok()) {
        problem("table " + name + ": undecodable row: " + row.status().to_string());
        return true;
      }
      ++rows;
      if (Status vs = validate_row(t, row.value()); !vs.ok()) problem("table " + name + ": " + vs.to_string());
      if (pk_key_of(t, row.value()) != key) problem("table " + name + ": row key differs from its primary key");
      if (Status fs = check_parents(t, row.value(), nullptr); !fs.ok()) problem("table " + name + ": " + fs.to_string());
      for (const IndexDef& idx : t.indexes) {
        auto e = tree(idx.root).get(index_key_of(t, idx, row.value(), key));
        if (!e.ok()) return e.status();
        if (!e.value().has_value()) problem("index " + name + "." + idx.name + ": missing entry for a row");
      }
      return true;
    });
    if (!s.ok()) problem("table " + name + ": " + s.to_string());
    rep.rows += rows;
    if (pk_rep->entries != rows) problem("table " + name + ": entry count mismatch");
    for (std::size_t i = 0; i < t.indexes.size(); ++i) {
      const IndexDef& idx = t.indexes[i];
      const std::vector<ColumnType> idx_types = types_of(t, idx.columns);
      std::uint64_t entries = 0;
      Bytes prev_prefix;
      bool have_prev = false;
      Status is = scan_raw(t, &idx, std::nullopt, std::nullopt, false, [&](const Bytes& key) -> Result<bool> {
        ++entries;
        Row idx_values;
        std::size_t pos = 0;
        if (Status ds = decode_key(key, pos, idx_types, idx_values); !ds.ok()) {
          problem("index " + name + "." + idx.name + ": undecodable entry");
          return true;
        }
        const Bytes prefix(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(pos));
        const Bytes pk_key(key.begin() + static_cast<std::ptrdiff_t>(pos), key.end());
        auto row = get_by_key(t, pk_key);
        if (!row.ok()) return row.status();
        if (!row.value().has_value()) {
          problem("index " + name + "." + idx.name + ": entry without a row");
        } else if (index_key_of(t, idx, *row.value(), pk_key) != key) {
          problem("index " + name + "." + idx.name + ": entry disagrees with its row");
        }
        if (idx.unique && have_prev && prefix == prev_prefix && !any_null(idx_values)) {
          problem("index " + name + "." + idx.name + ": duplicate in unique index");
        }
        prev_prefix = prefix;
        have_prev = true;
        return true;
      });
      if (!is.ok()) problem("index " + name + "." + idx.name + ": " + is.to_string());
      rep.index_entries += entries;
      if (entries != rows) problem("index " + name + "." + idx.name + ": entry count differs from row count");
      if (idx_reps[i] && idx_reps[i]->entries != entries) problem("index " + name + "." + idx.name + ": entry count mismatch");
    }
  }
  rep.pages_owned = owner.size();
  own(rep.pager.free_pages, "free list");
  const std::uint64_t page_count = rtxn_ ? rtxn_->page_count() : 0;
  for (PageNo p = 1; p < page_count; ++p) {
    if (owner.find(p) == owner.end()) problem("page " + std::to_string(p) + " is owned by nobody");
  }
  for (const auto& [p, who] : owner) {
    if (p == 0 || p >= page_count) problem("page " + std::to_string(p) + " owned by " + who + " is outside the file");
  }
  return Status();
}

// ---- Writer ----------------------------------------------------------------

Writer::Writer(Store& store, std::unique_ptr<WriteTxn> txn, std::shared_ptr<const Catalog> catalog)
    : Reader(store, nullptr), txn_(std::move(txn)) {
  wpages_ = std::make_unique<WriteTxnPages>(*txn_, store_.db().page_size());
  pages_ = wpages_.get();
  writer_ = wpages_.get();
  edited_ = std::make_shared<Catalog>(*catalog);
  catalog_ = edited_;
}

Writer::~Writer() { rollback(); }

Catalog& Writer::mutable_catalog() { return *edited_; }

Status Writer::commit() {
  if (finished_) return Status::invalid_argument("transaction finished");
  finished_ = true;
  const std::uint64_t cc = txn_->change_counter();
  Status s = txn_->commit();
  if (s.ok()) store_.catalog_committed(cc + 1, edited_);
  return s;
}

void Writer::rollback() {
  if (finished_) return;
  finished_ = true;
  txn_->rollback();
}

Status Writer::save_table(const TableDef& t) {
  schema_changed_ = true;
  return tree(kCatalogRoot).put(table_key(t.name), encode_row(encode_table_def(t)));
}

Status Writer::save_version() {
  schema_changed_ = true;
  Row r = {Value::integer(static_cast<std::int64_t>(edited_->schema_version))};
  return tree(kCatalogRoot).put(Bytes{kVersionKey}, encode_row(r));
}

Status Writer::set_schema_version(std::uint64_t version) {
  if (finished_) return Status::invalid_argument("transaction finished");
  edited_->schema_version = version;
  return save_version();
}

Status Writer::put_index_entries(const TableDef& t, const Row& row, const Bytes& pk_key, const Row* old_row) {
  for (const IndexDef& idx : t.indexes) {
    if (old_row && rows_equal(project(t, row, idx.columns), project(t, *old_row, idx.columns))) continue;
    if (Status s = tree(idx.root).put(index_key_of(t, idx, row, pk_key), {}); !s.ok()) return s;
  }
  return Status();
}

Status Writer::erase_index_entries(const TableDef& t, const Row& row, const Bytes& pk_key) {
  for (const IndexDef& idx : t.indexes) {
    bool existed = false;
    if (Status s = tree(idx.root).erase(index_key_of(t, idx, row, pk_key), existed); !s.ok()) return s;
    if (!existed) return Status::corrupt("index " + idx.name + " lacks the entry for a row");
  }
  return Status();
}

Status Writer::insert_unchecked(const TableDef& t, const Row& row, bool check_fks) {
  if (Status s = validate_row(t, row); !s.ok()) return s;
  const Bytes pk_key = pk_key_of(t, row);
  BTree pk = tree(t.root);
  if (pk_key.size() > pk.max_key_bytes()) return Status::invalid_argument("primary key of " + t.name + " too long");
  auto existing = pk.get(pk_key);
  if (!existing.ok()) return existing.status();
  if (existing.value().has_value()) return Status::constraint("primary key of " + t.name);
  for (const IndexDef& idx : t.indexes) {
    const Row values = project(t, row, idx.columns);
    if (encode_key(values).size() + pk_key.size() > pk.max_key_bytes()) {
      return Status::invalid_argument("index key " + idx.name + " too long");
    }
    if (!idx.unique || any_null(values)) continue;
    auto dup = index_prefix_exists(t, idx, values);
    if (!dup.ok()) return dup.status();
    if (dup.value()) return Status::constraint("unique index " + idx.name + " on " + t.name);
  }
  if (check_fks) {
    if (Status s = check_parents(t, row, nullptr); !s.ok()) return s;
  }
  if (Status s = pk.put(pk_key, encode_row(row)); !s.ok()) return s;
  return put_index_entries(t, row, pk_key, nullptr);
}

Status Writer::insert(const std::string& table, const Row& row) {
  if (finished_) return Status::invalid_argument("transaction finished");
  const TableDef* t = catalog_->table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  return insert_unchecked(*t, row, true);
}

Status Writer::update(const std::string& table, const Row& row) {
  if (finished_) return Status::invalid_argument("transaction finished");
  const TableDef* t = catalog_->table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  if (Status s = validate_row(*t, row); !s.ok()) return s;
  const Bytes pk_key = pk_key_of(*t, row);
  auto old = get_by_key(*t, pk_key);
  if (!old.ok()) return old.status();
  if (!old.value().has_value()) return Status::not_found("no row with that primary key in " + table);
  const Row& old_row = *old.value();
  if (rows_equal(old_row, row)) return Status();
  BTree pk = tree(t->root);
  for (const IndexDef& idx : t->indexes) {
    const Row values = project(*t, row, idx.columns);
    if (rows_equal(values, project(*t, old_row, idx.columns))) continue;
    if (encode_key(values).size() + pk_key.size() > pk.max_key_bytes()) {
      return Status::invalid_argument("index key " + idx.name + " too long");
    }
    if (!idx.unique || any_null(values)) continue;
    auto dup = index_prefix_exists(*t, idx, values);
    if (!dup.ok()) return dup.status();
    if (dup.value()) return Status::constraint("unique index " + idx.name + " on " + table);
  }
  if (Status s = check_parents(*t, row, &old_row); !s.ok()) return s;
  if (Status s = check_no_children(*t, old_row, &row); !s.ok()) return s;
  for (const IndexDef& idx : t->indexes) {
    if (rows_equal(project(*t, row, idx.columns), project(*t, old_row, idx.columns))) continue;
    bool existed = false;
    if (Status s = tree(idx.root).erase(index_key_of(*t, idx, old_row, pk_key), existed); !s.ok()) return s;
    if (!existed) return Status::corrupt("index " + idx.name + " lacks the entry for a row");
  }
  if (Status s = pk.put(pk_key, encode_row(row)); !s.ok()) return s;
  return put_index_entries(*t, row, pk_key, &old_row);
}

Status Writer::remove(const std::string& table, const Row& primary_key) {
  if (finished_) return Status::invalid_argument("transaction finished");
  auto old = get(table, primary_key);
  if (!old.ok()) return old.status();
  if (!old.value().has_value()) return Status::not_found("no row with that primary key in " + table);
  const TableDef& t = *catalog_->table(table);
  const Row& old_row = *old.value();
  if (Status s = check_no_children(t, old_row, nullptr); !s.ok()) return s;
  const Bytes pk_key = encode_key(primary_key);
  if (Status s = erase_index_entries(t, old_row, pk_key); !s.ok()) return s;
  bool existed = false;
  if (Status s = tree(t.root).erase(pk_key, existed); !s.ok()) return s;
  if (!existed) return Status::corrupt("row vanished during remove");
  return Status();
}

Status Writer::create_table(TableDef def) {
  if (finished_) return Status::invalid_argument("transaction finished");
  if (edited_->table(def.name) != nullptr) return Status::already_exists("table " + def.name);
  for (const auto& c : def.primary_key) {
    const int ci = def.column_index(c);
    if (ci >= 0) def.columns[static_cast<std::size_t>(ci)].nullable = false;
  }
  if (Status s = validate_table_def(*edited_, def); !s.ok()) return s;
  auto root = BTree::create(*writer_);
  if (!root.ok()) return root.status();
  def.root = root.value();
  for (IndexDef& idx : def.indexes) {
    auto r = BTree::create(*writer_);
    if (!r.ok()) return r.status();
    idx.root = r.value();
  }
  edited_->tables[def.name] = def;
  return save_table(def);
}

Status Writer::drop_table(const std::string& table) {
  if (finished_) return Status::invalid_argument("transaction finished");
  const TableDef* t = edited_->table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  for (const auto& [cname, child] : edited_->tables) {
    if (cname == table) continue;
    for (const ForeignKeyDef& fk : child.foreign_keys) {
      if (fk.ref_table == table) {
        return Status::invalid_argument("table " + table + " is referenced by foreign key " + fk.name + " on " + cname);
      }
    }
  }
  if (Status s = BTree::destroy(*writer_, t->root); !s.ok()) return s;
  for (const IndexDef& idx : t->indexes) {
    if (Status s = BTree::destroy(*writer_, idx.root); !s.ok()) return s;
  }
  edited_->tables.erase(table);
  schema_changed_ = true;
  bool existed = false;
  return tree(kCatalogRoot).erase(table_key(table), existed);
}

Status Writer::create_index(const std::string& table, IndexDef def) {
  if (finished_) return Status::invalid_argument("transaction finished");
  TableDef* t = nullptr;
  if (auto it = edited_->tables.find(table); it != edited_->tables.end()) t = &it->second;
  if (t == nullptr) return Status::not_found("no table " + table);
  if (Status s = validate_index_def(*t, def, nullptr); !s.ok()) return s;
  auto root = BTree::create(*writer_);
  if (!root.ok()) return root.status();
  def.root = root.value();
  BTree it = tree(def.root);
  // Rows arrive in primary-key order, so duplicates in a unique index are
  // found by prefix lookup before each put.
  Status s = scan_raw(*t, nullptr, std::nullopt, std::nullopt, false, [&](const Bytes& pk_key) -> Result<bool> {
    auto row = get_by_key(*t, pk_key);
    if (!row.ok()) return row.status();
    if (!row.value().has_value()) return Status::corrupt("row vanished during index build");
    const Row values = project(*t, *row.value(), def.columns);
    const Bytes prefix = encode_key(values);
    if (prefix.size() + pk_key.size() > it.max_key_bytes()) {
      return Status::invalid_argument("index key " + def.name + " too long");
    }
    if (def.unique && !any_null(values)) {
      auto c = it.cursor();
      if (Status cs = c.seek(prefix); !cs.ok()) return cs;
      if (c.valid() && starts_with(c.key(), prefix)) {
        return Status::constraint("unique index " + def.name + " on " + table + ": existing rows are not unique");
      }
    }
    Bytes k = prefix;
    k.insert(k.end(), pk_key.begin(), pk_key.end());
    if (Status ps = it.put(k, {}); !ps.ok()) return ps;
    return true;
  });
  if (!s.ok()) {
    // Leave the transaction consistent for a constraint failure.
    if (s.code() == ErrorCode::Constraint || s.code() == ErrorCode::InvalidArgument) {
      if (Status ds = BTree::destroy(*writer_, def.root); !ds.ok()) return ds;
    }
    return s;
  }
  t->indexes.push_back(std::move(def));
  return save_table(*t);
}

Status Writer::drop_index(const std::string& table, const std::string& index) {
  if (finished_) return Status::invalid_argument("transaction finished");
  TableDef* t = nullptr;
  if (auto it = edited_->tables.find(table); it != edited_->tables.end()) t = &it->second;
  if (t == nullptr) return Status::not_found("no table " + table);
  auto pos = std::find_if(t->indexes.begin(), t->indexes.end(), [&](const IndexDef& i) { return i.name == index; });
  if (pos == t->indexes.end()) return Status::not_found("no index " + index + " on " + table);
  // The index must not be the only support of a foreign key on this table
  // or the only unique target of a foreign key from another table.
  TableDef without = *t;
  without.indexes.erase(without.indexes.begin() + (pos - t->indexes.begin()));
  for (const ForeignKeyDef& fk : without.foreign_keys) {
    if (!pk_supports(without, fk.columns) && supporting_index(without, fk.columns) == nullptr) {
      return Status::invalid_argument("index " + index + " supports foreign key " + fk.name);
    }
  }
  if (pos->unique) {
    bool other_unique = pos->columns == t->primary_key;
    for (const IndexDef& idx : without.indexes) {
      if (idx.unique && idx.columns == pos->columns) other_unique = true;
    }
    for (const auto& [cname, child] : edited_->tables) {
      for (const ForeignKeyDef& fk : child.foreign_keys) {
        if (fk.ref_table == table && fk.ref_columns == pos->columns && !other_unique) {
          return Status::invalid_argument("index " + index + " is the target of foreign key " + fk.name + " on " + cname);
        }
      }
    }
  }
  if (Status s = BTree::destroy(*writer_, pos->root); !s.ok()) return s;
  t->indexes.erase(pos);
  return save_table(*t);
}

Status Writer::verify_foreign_keys(const std::string& table) {
  const TableDef* t = catalog_->table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  if (t->foreign_keys.empty()) return Status();
  return scan_raw(*t, nullptr, std::nullopt, std::nullopt, false, [&](const Bytes& key) -> Result<bool> {
    auto row = get_by_key(*t, key);
    if (!row.ok()) return row.status();
    if (!row.value().has_value()) return Status::corrupt("row vanished during foreign key verification");
    if (Status s = check_parents(*t, *row.value(), nullptr); !s.ok()) return s;
    return true;
  });
}

// ---- Store -----------------------------------------------------------------

Store::Store(std::unique_ptr<Db> db) : db_(std::move(db)) {}
Store::~Store() = default;

Result<std::unique_ptr<Store>> Store::open(Vfs& vfs, std::string path, DbOptions options) {
  auto db = Db::open(vfs, std::move(path), options);
  if (!db.ok()) return db.status();
  std::unique_ptr<Store> store(new Store(std::move(db.value())));
  bool fresh = false;
  {
    auto r = store->db_->begin_read();
    if (!r.ok()) return r.status();
    fresh = r.value()->page_count() == 1;
  }
  if (fresh) {
    auto w = store->db_->begin_write();
    if (!w.ok()) return w.status();
    if (w.value()->page_count() == 1) {
      WriteTxnPages pages(*w.value(), store->db_->page_size());
      auto root = BTree::create(pages);
      if (!root.ok()) return root.status();
      if (root.value() != kCatalogRoot) return Status::corrupt("catalog root is not page 1");
      BTree cat(pages, &pages, kCatalogRoot);
      Row r = {Value::integer(0)};
      if (Status s = cat.put(Bytes{kVersionKey}, encode_row(r)); !s.ok()) return s;
      if (Status s = w.value()->commit(); !s.ok()) return s;
    } else {
      w.value()->rollback();
    }
  }
  auto reader = store->begin_read();  // loads and validates the catalog
  if (!reader.ok()) return reader.status();
  return store;
}

Result<std::shared_ptr<const Catalog>> Store::catalog_for(std::uint64_t change_counter, PageReader& pages) {
  {
    std::lock_guard<std::mutex> lock(catalog_mu_);
    if (catalog_ && catalog_change_counter_ == change_counter) return catalog_;
  }
  auto cat = std::make_shared<Catalog>();
  if (Status s = load_catalog(pages, *cat); !s.ok()) return s;
  std::shared_ptr<const Catalog> out = cat;
  std::lock_guard<std::mutex> lock(catalog_mu_);
  if (!catalog_ || catalog_change_counter_ <= change_counter) {
    catalog_ = out;
    catalog_change_counter_ = change_counter;
  }
  return out;
}

void Store::catalog_committed(std::uint64_t change_counter, std::shared_ptr<const Catalog> catalog) {
  std::lock_guard<std::mutex> lock(catalog_mu_);
  if (!catalog_ || catalog_change_counter_ <= change_counter) {
    catalog_ = std::move(catalog);
    catalog_change_counter_ = change_counter;
  }
}

Result<std::unique_ptr<Reader>> Store::begin_read() {
  auto txn = db_->begin_read();
  if (!txn.ok()) return txn.status();
  auto pages = std::make_unique<ReadTxnPages>(*txn.value(), db_->page_size());
  auto cat = catalog_for(txn.value()->change_counter(), *pages);
  if (!cat.ok()) return cat.status();
  std::unique_ptr<Reader> r(new Reader(*this, cat.value()));
  r->rtxn_ = std::move(txn.value());
  r->rpages_ = std::move(pages);
  r->pages_ = r->rpages_.get();
  return r;
}

Result<std::unique_ptr<Writer>> Store::begin_write() {
  auto txn = db_->begin_write();
  if (!txn.ok()) return txn.status();
  WriteTxnPages pages(*txn.value(), db_->page_size());
  auto cat = catalog_for(txn.value()->change_counter(), pages);
  if (!cat.ok()) return cat.status();
  return std::unique_ptr<Writer>(new Writer(*this, std::move(txn.value()), cat.value()));
}

Result<StoreCheckReport> Store::check() {
  StoreCheckReport rep;
  auto pager = db_->check();
  if (!pager.ok()) return pager.status();
  rep.pager = std::move(pager.value());
  for (const auto& p : rep.pager.problems) rep.problems.push_back("pager: " + p);
  auto reader = begin_read();
  if (!reader.ok()) return reader.status();
  if (Status s = reader.value()->check_into(rep); !s.ok()) return s;
  rep.ok = rep.problems.empty();
  return rep;
}

Result<Dump> Store::dump() {
  auto reader = begin_read();
  if (!reader.ok()) return reader.status();
  Dump d;
  d.schema_version = reader.value()->catalog().schema_version;
  for (const auto& [name, t] : reader.value()->catalog().tables) {
    TableDump td;
    td.def = t;
    td.def.root = 0;
    for (IndexDef& idx : td.def.indexes) idx.root = 0;
    auto rows = reader.value()->scan_all(name);
    if (!rows.ok()) return rows.status();
    td.rows = std::move(rows.value());
    d.tables.push_back(std::move(td));
  }
  return d;
}

Status Store::restore(const Dump& dump) {
  auto w = begin_write();
  if (!w.ok()) return w.status();
  Writer& writer = *w.value();
  if (!writer.catalog().tables.empty()) return Status::invalid_argument("restore needs an empty store");
  // Create tables parents first.
  std::unordered_set<std::string> created;
  std::vector<const TableDump*> pending;
  for (const TableDump& td : dump.tables) pending.push_back(&td);
  std::vector<const TableDump*> order;
  while (!pending.empty()) {
    bool progress = false;
    for (auto it = pending.begin(); it != pending.end();) {
      const TableDump& td = **it;
      bool ready = true;
      for (const ForeignKeyDef& fk : td.def.foreign_keys) {
        if (fk.ref_table != td.def.name && !created.count(fk.ref_table)) ready = false;
      }
      if (ready) {
        created.insert(td.def.name);
        order.push_back(&td);
        it = pending.erase(it);
        progress = true;
      } else {
        ++it;
      }
    }
    if (!progress) return Status::invalid_argument("restore: foreign keys form a cycle between tables");
  }
  for (const TableDump* td : order) {
    if (Status s = writer.create_table(td->def); !s.ok()) return s;
  }
  for (const TableDump* td : order) {
    const TableDef& t = *writer.catalog().table(td->def.name);
    for (const Row& row : td->rows) {
      if (Status s = writer.insert_unchecked(t, row, false); !s.ok()) return s;
    }
  }
  for (const TableDump* td : order) {
    if (Status s = writer.verify_foreign_keys(td->def.name); !s.ok()) return s;
  }
  if (Status s = writer.set_schema_version(dump.schema_version); !s.ok()) return s;
  return writer.commit();
}

Status Store::close() { return db_->close(); }

}  // namespace archivum::engine
