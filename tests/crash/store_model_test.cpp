// Model-based test of the typed layer. A naive model (std::map of rows per
// table plus the table definitions) receives every operation the store
// receives: inserts, updates, deletes with deliberate constraint
// violations, index creation and removal, rollbacks, checkpoints. After
// every committed transaction the full state is compared: every table by
// primary key, every index in index order, random bounded range scans in
// both directions, and the store's invariant checker. The run is seeded
// and reproducible (ARCHIVUM_SEED) and its length scales with
// ARCHIVUM_CRASH_ITERS. It runs over the crash shim: crashes land at random
// points, the store is reopened and compared with the committed model, an
// in-flight commit being accepted as either wholly present or absent.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

#include "archivum/engine/store.h"
#include "archivum/testing/fault_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

std::uint64_t splitmix(std::uint64_t& x) {
  std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
struct Rng {
  std::uint64_t s;
  explicit Rng(std::uint64_t seed) : s(seed) {}
  std::uint64_t next() { return splitmix(s); }
  std::uint64_t below(std::uint64_t n) { return n == 0 ? 0 : next() % n; }
  bool chance(std::uint64_t one_in) { return below(one_in) == 0; }
};

// ---- the model ---------------------------------------------------------------

struct ModelTable {
  TableDef def;
  std::map<Bytes, Row> rows;  // by encoded primary key
};
struct Model {
  std::uint64_t version = 0;
  std::map<std::string, ModelTable> tables;
};

Row project(const TableDef& t, const Row& row, const std::vector<std::string>& cols) {
  Row out;
  for (const auto& c : cols) out.push_back(row[static_cast<std::size_t>(t.column_index(c))]);
  return out;
}
bool any_null(const Row& r) {
  return std::any_of(r.begin(), r.end(), [](const Value& v) { return v.is_null(); });
}
bool rows_equal(const Row& a, const Row& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}
bool check_holds(const CheckDef& c, const Value& v) {
  if (v.is_null()) return true;
  auto cmp = [&](const Value& o) { return Value::compare(v, o); };
  switch (c.op) {
    case CheckOp::Eq:
      return cmp(c.operands[0]) == 0;
    case CheckOp::Ne:
      return cmp(c.operands[0]) != 0;
    case CheckOp::Lt:
      return cmp(c.operands[0]) < 0;
    case CheckOp::Le:
      return cmp(c.operands[0]) <= 0;
    case CheckOp::Gt:
      return cmp(c.operands[0]) > 0;
    case CheckOp::Ge:
      return cmp(c.operands[0]) >= 0;
    case CheckOp::In:
      return std::any_of(c.operands.begin(), c.operands.end(), [&](const Value& o) { return cmp(o) == 0; });
  }
  return false;
}

// Does any row of `t` (other than `except`) carry `values` in `cols`?
bool model_has(const ModelTable& t, const std::vector<std::string>& cols, const Row& values, const Bytes* except) {
  for (const auto& [k, r] : t.rows) {
    if (except && k == *except) continue;
    if (rows_equal(project(t.def, r, cols), values)) return true;
  }
  return false;
}

std::uint32_t g_max_key = 0;

bool key_too_long(const TableDef& t, const Row& row) {
  const std::size_t pk = encode_key(project(t, row, t.primary_key)).size();
  if (pk > g_max_key) return true;
  for (const IndexDef& idx : t.indexes) {
    if (encode_key(project(t, row, idx.columns)).size() + pk > g_max_key) return true;
  }
  return false;
}

// Whether the model accepts the row (ignoring the primary key).
bool model_row_valid(const Model& m, const ModelTable& t, const Row& row, const Bytes* self) {
  if (key_too_long(t.def, row)) return false;
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (row[i].is_null() && !t.def.columns[i].nullable) return false;
  }
  for (const CheckDef& c : t.def.checks) {
    if (!check_holds(c, row[static_cast<std::size_t>(t.def.column_index(c.column))])) return false;
  }
  for (const IndexDef& idx : t.def.indexes) {
    if (!idx.unique) continue;
    const Row v = project(t.def, row, idx.columns);
    if (any_null(v)) continue;
    if (model_has(t, idx.columns, v, self)) return false;
  }
  for (const ForeignKeyDef& fk : t.def.foreign_keys) {
    const Row v = project(t.def, row, fk.columns);
    if (any_null(v)) continue;
    const ModelTable& parent = m.tables.at(fk.ref_table);
    if (!model_has(parent, fk.ref_columns, v, nullptr)) return false;
  }
  return true;
}

// Whether some child references the given values of `t`'s row.
bool model_referenced(const Model& m, const ModelTable& t, const Row& row, const Row* new_row) {
  for (const auto& [cname, child] : m.tables) {
    for (const ForeignKeyDef& fk : child.def.foreign_keys) {
      if (fk.ref_table != t.def.name) continue;
      const Row v = project(t.def, row, fk.ref_columns);
      if (any_null(v)) continue;
      if (new_row && rows_equal(v, project(t.def, *new_row, fk.ref_columns))) continue;
      if (model_has(child, fk.columns, v, nullptr)) return true;
    }
  }
  return false;
}

// ---- the schema --------------------------------------------------------------

TableDef parents_def() {
  TableDef t;
  t.name = "parents";
  t.columns = {{"id", ColumnType::Integer, false, 0},   {"code", ColumnType::Text, false, 0},
               {"u", ColumnType::Integer, false, 0},    {"amount", ColumnType::Decimal, true, 2},
               {"flag", ColumnType::Boolean, false, 0}, {"when_", ColumnType::Timestamp, true, 0},
               {"dev", ColumnType::Uuid, true, 0},      {"data", ColumnType::Blob, true, 0}};
  t.primary_key = {"id"};
  t.indexes = {{"parents_code", {"code"}, true, 0}, {"parents_u", {"u"}, true, 0}, {"parents_flag", {"flag", "when_"}, false, 0}};
  t.checks = {{"amount_positive", "amount", CheckOp::Gt, {Value::decimal(0)}}};
  return t;
}
TableDef children_def() {
  TableDef t;
  t.name = "children";
  t.columns = {{"parent_id", ColumnType::Integer, false, 0}, {"seq", ColumnType::Integer, false, 0},
               {"parent_u", ColumnType::Integer, true, 0},   {"kind", ColumnType::Text, false, 0},
               {"note", ColumnType::Text, true, 0}};
  t.primary_key = {"parent_id", "seq"};
  t.indexes = {{"children_pu", {"parent_u"}, false, 0}, {"children_kind", {"kind", "note"}, false, 0}};
  t.foreign_keys = {{"fk_parent", {"parent_id"}, "parents", {"id"}}, {"fk_parent_u", {"parent_u"}, "parents", {"u"}}};
  t.checks = {{"kind_known", "kind", CheckOp::In, {Value::text("a"), Value::text("b"), Value::text("c")}}};
  return t;
}
TableDef tags_def() {
  TableDef t;
  t.name = "tags";
  t.columns = {{"name", ColumnType::Text, false, 0}, {"child_parent", ColumnType::Integer, true, 0},
               {"child_seq", ColumnType::Integer, true, 0}, {"weight", ColumnType::Integer, false, 0}};
  t.primary_key = {"name"};
  t.indexes = {{"tags_child", {"child_parent", "child_seq"}, false, 0}};
  t.foreign_keys = {{"fk_child", {"child_parent", "child_seq"}, "children", {"parent_id", "seq"}}};
  t.checks = {{"name_nonempty", "name", CheckOp::Ne, {Value::text("")}}, {"weight_range", "weight", CheckOp::Le, {Value::integer(100)}}};
  return t;
}

// ---- random values -------------------------------------------------------------

Value random_value(Rng& rng, const ColumnDef& c, const Model& m) {
  if (c.nullable ? rng.chance(5) : rng.chance(40)) return Value::null();
  switch (c.type) {
    case ColumnType::Integer:
      if (c.name == "id" || c.name == "parent_id" || c.name == "child_parent") return Value::integer(static_cast<std::int64_t>(rng.below(30)));
      if (c.name == "seq" || c.name == "child_seq") return Value::integer(static_cast<std::int64_t>(rng.below(4)));
      if (c.name == "u" || c.name == "parent_u") return Value::integer(static_cast<std::int64_t>(rng.below(40)));
      if (c.name == "weight") return Value::integer(static_cast<std::int64_t>(rng.below(120)) - 10);
      return Value::integer(static_cast<std::int64_t>(rng.next()));
    case ColumnType::Decimal:
      return Value::decimal(static_cast<std::int64_t>(rng.below(2000)) - 100);
    case ColumnType::Text: {
      if (c.name == "kind") {
        static const char* kinds[] = {"a", "b", "c", "d"};
        return Value::text(kinds[rng.below(4)]);
      }
      std::string s;
      const std::size_t len = rng.below(c.name == "code" ? 4 : 9);
      for (std::size_t i = 0; i < len; ++i) {
        const auto pick = rng.below(20);
        if (pick == 0) s += '\0';
        else if (pick == 1) s += "\xC3\xA9";  // é
        else s += static_cast<char>('a' + rng.below(4));
      }
      if (c.name == "name" && rng.chance(30)) s.clear();
      return Value::text(s);
    }
    case ColumnType::Blob: {
      const std::size_t len = rng.chance(10) ? 1500 + rng.below(3000) : rng.below(40);
      Bytes b(len);
      for (auto& x : b) x = static_cast<std::byte>(rng.next() & 0xFF);
      return Value::blob(std::move(b));
    }
    case ColumnType::Timestamp:
      return Value::timestamp(static_cast<std::int64_t>(rng.below(1000)) - 500);
    case ColumnType::Boolean:
      return Value::boolean(rng.chance(2));
    case ColumnType::Uuid: {
      UuidBytes u{};
      const auto pool = rng.below(6);
      for (std::size_t i = 0; i < 16; ++i) u[i] = static_cast<std::byte>(pool * 31 + i);
      return Value::uuid(u);
    }
  }
  (void)m;
  return Value::null();
}

Row random_row(Rng& rng, const TableDef& t, const Model& m) {
  Row r;
  for (const ColumnDef& c : t.columns) r.push_back(random_value(rng, c, m));
  return r;
}

std::string show(const Row& r) {
  std::string s = "(";
  for (std::size_t i = 0; i < r.size(); ++i) s += (i ? ", " : "") + r[i].to_string();
  return s + ")";
}

// ---- comparison ----------------------------------------------------------------

Bytes composite_key(const TableDef& t, const IndexDef* idx, const Row& row) {
  Bytes k;
  if (idx) k = encode_key(project(t, row, idx->columns));
  const Bytes pk = encode_key(project(t, row, t.primary_key));
  k.insert(k.end(), pk.begin(), pk.end());
  return k;
}
bool starts_with(const Bytes& key, const Bytes& prefix) {
  return key.size() >= prefix.size() && std::memcmp(key.data(), prefix.data(), prefix.size()) == 0;
}
bool key_less(const Bytes& a, const Bytes& b) { return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end()); }

// Rows of `t` ordered by the given index (or the primary key), filtered by the bounds.
std::vector<Row> model_scan(const ModelTable& t, const IndexDef* idx, const std::optional<Bound>& lower,
                            const std::optional<Bound>& upper, bool reverse) {
  std::vector<std::pair<Bytes, const Row*>> entries;
  for (const auto& [k, r] : t.rows) entries.emplace_back(composite_key(t.def, idx, r), &r);
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return key_less(a.first, b.first); });
  const Bytes lo = lower ? encode_key(lower->values) : Bytes();
  const Bytes hi = upper ? encode_key(upper->values) : Bytes();
  std::vector<Row> out;
  for (const auto& [k, r] : entries) {
    if (lower) {
      const bool in = lower->inclusive ? !key_less(k, lo) : (key_less(lo, k) && !starts_with(k, lo));
      if (!in) continue;
    }
    if (upper) {
      const bool in = upper->inclusive ? (key_less(k, hi) || starts_with(k, hi)) : key_less(k, hi);
      if (!in) continue;
    }
    out.push_back(*r);
  }
  if (reverse) std::reverse(out.begin(), out.end());
  return out;
}

std::string diff_rows(const std::vector<Row>& engine, const std::vector<Row>& model) {
  if (engine.size() != model.size()) {
    return "row count " + std::to_string(engine.size()) + " != model " + std::to_string(model.size());
  }
  for (std::size_t i = 0; i < engine.size(); ++i) {
    if (!rows_equal(engine[i], model[i])) return "row " + std::to_string(i) + ": " + show(engine[i]) + " != " + show(model[i]);
  }
  return "";
}

std::vector<std::string> compare(Reader& rd, const Model& m, Rng& rng) {
  std::vector<std::string> problems;
  if (rd.catalog().schema_version != m.version) problems.push_back("schema version differs");
  if (rd.catalog().tables.size() != m.tables.size()) problems.push_back("table count differs");
  for (const auto& [name, mt] : m.tables) {
    const TableDef* t = rd.catalog().table(name);
    if (t == nullptr) {
      problems.push_back("table " + name + " missing");
      continue;
    }
    if (t->indexes.size() != mt.def.indexes.size()) problems.push_back("table " + name + ": index count differs");
    auto all = rd.scan_all(name);
    if (!all.ok()) {
      problems.push_back("scan " + name + ": " + all.status().to_string());
      continue;
    }
    if (std::string d = diff_rows(all.value(), model_scan(mt, nullptr, std::nullopt, std::nullopt, false)); !d.empty()) {
      problems.push_back("table " + name + ": " + d);
    }
    for (const IndexDef& idx : mt.def.indexes) {
      const IndexDef* eidx = t->index(idx.name);
      if (eidx == nullptr || eidx->columns != idx.columns || eidx->unique != idx.unique) {
        problems.push_back("index " + name + "." + idx.name + " differs");
        continue;
      }
      auto by = rd.scan_all(name, idx.name);
      if (!by.ok()) {
        problems.push_back("scan " + idx.name + ": " + by.status().to_string());
        continue;
      }
      if (std::string d = diff_rows(by.value(), model_scan(mt, &idx, std::nullopt, std::nullopt, false)); !d.empty()) {
        problems.push_back("index " + name + "." + idx.name + ": " + d);
      }
    }
    // Random bounded scans, forward and reverse, over the key or an index.
    for (int k = 0; k < 3 && !mt.rows.empty(); ++k) {
      const IndexDef* idx = mt.def.indexes.empty() || rng.chance(3) ? nullptr : &mt.def.indexes[rng.below(mt.def.indexes.size())];
      std::vector<std::string> key_cols = idx ? idx->columns : mt.def.primary_key;
      if (idx) key_cols.insert(key_cols.end(), mt.def.primary_key.begin(), mt.def.primary_key.end());
      auto random_bound = [&]() -> std::optional<Bound> {
        if (rng.chance(3)) return std::nullopt;
        auto it = mt.rows.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rng.below(mt.rows.size())));
        std::vector<std::string> cols(key_cols.begin(), key_cols.begin() + static_cast<std::ptrdiff_t>(1 + rng.below(key_cols.size())));
        return Bound{project(mt.def, it->second, cols), rng.chance(2)};
      };
      const auto lower = random_bound();
      const auto upper = random_bound();
      const bool reverse = rng.chance(2);
      auto got = rd.scan_all(name, idx ? idx->name : "", lower, upper, reverse);
      if (!got.ok()) {
        problems.push_back("bounded scan: " + got.status().to_string());
        continue;
      }
      if (std::string d = diff_rows(got.value(), model_scan(mt, idx, lower, upper, reverse)); !d.empty()) {
        problems.push_back("bounded scan of " + name + (idx ? "." + idx->name : "") + (reverse ? " reverse" : "") + ": " + d);
      }
    }
    // Point lookups for a few keys, present and absent.
    for (int k = 0; k < 3; ++k) {
      Row pk;
      const Row* expect = nullptr;
      if (!mt.rows.empty() && !rng.chance(3)) {
        auto it = mt.rows.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(rng.below(mt.rows.size())));
        pk = project(mt.def, it->second, mt.def.primary_key);
        expect = &it->second;
      } else {
        Row r = random_row(rng, mt.def, m);
        pk = project(mt.def, r, mt.def.primary_key);
        if (any_null(pk)) continue;
        auto it = mt.rows.find(encode_key(pk));
        if (it != mt.rows.end()) expect = &it->second;
      }
      auto g = rd.get(name, pk);
      if (!g.ok()) {
        problems.push_back("get: " + g.status().to_string());
      } else if (g.value().has_value() != (expect != nullptr) || (expect && !rows_equal(*g.value(), *expect))) {
        problems.push_back("get " + show(pk) + " in " + name + " disagrees with the model");
      }
    }
  }
  return problems;
}

DbOptions opts() {
  DbOptions o;
  o.page_size = 1024;
  o.cache_pages = 32;
  o.checkpoint_threshold_frames = 60;
  return o;
}

std::vector<std::string> compare_store(Store& store, const Model& m, Rng& rng, bool full_check) {
  auto rd = store.begin_read();
  if (!rd.ok()) return {"begin_read: " + rd.status().to_string()};
  auto problems = compare(*rd.value(), m, rng);
  rd.value().reset();
  if (full_check) {
    auto rep = store.check();
    if (!rep.ok()) {
      problems.push_back("check: " + rep.status().to_string());
    } else {
      for (const auto& p : rep.value().problems) problems.push_back("check: " + p);
    }
  }
  return problems;
}

// One random operation applied to both. Returns the store's status; the
// model is updated only when the store accepted the operation, and a
// disagreement between the model's verdict and the store's is recorded.
Status apply_random_op(Rng& rng, Writer& w, Model& m, std::vector<std::string>& disagreements) {
  const std::uint64_t pick = rng.below(100);
  std::vector<std::string> names;
  for (const auto& [n, t] : m.tables) names.push_back(n);
  ModelTable& mt = m.tables.at(names[rng.below(names.size())]);
  const TableDef& t = mt.def;
  auto verdict = [&](const char* what, Status st, bool model_ok) -> Status {
    if (st.code() == ErrorCode::Crashed || st.code() == ErrorCode::IoError) return st;
    if (st.ok() != model_ok) {
      disagreements.push_back(std::string(what) + " on " + t.name + ": store said " + st.to_string() +
                              ", model said " + (model_ok ? "ok" : "violation"));
    } else if (!st.ok() && st.code() != ErrorCode::Constraint && st.code() != ErrorCode::InvalidArgument &&
               st.code() != ErrorCode::AlreadyExists) {
      disagreements.push_back(std::string(what) + " on " + t.name + ": unexpected " + st.to_string());
    }
    return Status();
  };
  if (pick < 45) {
    Row row = random_row(rng, t, m);
    const Row pk = project(t, row, t.primary_key);
    const bool model_ok = !any_null(pk) && !mt.rows.count(encode_key(pk)) && model_row_valid(m, mt, row, nullptr);
    Status st = w.insert(t.name, row);
    if (st.ok()) mt.rows[encode_key(pk)] = row;
    return verdict("insert", st, model_ok);
  }
  if (pick < 70 && !mt.rows.empty()) {
    auto it = mt.rows.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(rng.below(mt.rows.size())));
    Row row = it->second;
    // Change one or two non-key columns.
    for (int k = 0; k < 1 + static_cast<int>(rng.below(2)); ++k) {
      const std::size_t ci = rng.below(t.columns.size());
      if (std::find(t.primary_key.begin(), t.primary_key.end(), t.columns[ci].name) != t.primary_key.end()) continue;
      row[ci] = random_value(rng, t.columns[ci], m);
    }
    const bool model_ok = model_row_valid(m, mt, row, &it->first) && !model_referenced(m, mt, it->second, &row);
    Status st = w.update(t.name, row);
    if (st.ok()) it->second = row;
    return verdict("update", st, model_ok);
  }
  if (pick < 85 && !mt.rows.empty()) {
    auto it = mt.rows.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(rng.below(mt.rows.size())));
    const bool model_ok = !model_referenced(m, mt, it->second, nullptr);
    Status st = w.remove(t.name, project(t, it->second, t.primary_key));
    if (st.ok()) mt.rows.erase(it);
    return verdict("remove", st, model_ok);
  }
  if (pick < 92) {
    // Create an index on one or two random columns; unique one time in three.
    IndexDef idx;
    idx.name = "x_" + std::to_string(rng.below(1000000));
    idx.columns.push_back(t.columns[rng.below(t.columns.size())].name);
    if (rng.chance(2)) {
      const std::string c = t.columns[rng.below(t.columns.size())].name;
      if (c != idx.columns[0]) idx.columns.push_back(c);
    }
    idx.unique = rng.chance(3);
    bool model_ok = t.index(idx.name) == nullptr;
    for (const auto& [k, r] : mt.rows) {
      if (encode_key(project(t, r, idx.columns)).size() + k.size() > g_max_key) model_ok = false;
    }
    if (idx.unique) {
      std::vector<Row> seen;
      for (const auto& [k, r] : mt.rows) {
        const Row v = project(t, r, idx.columns);
        if (any_null(v)) continue;
        for (const Row& s : seen) {
          if (rows_equal(s, v)) model_ok = false;
        }
        seen.push_back(v);
      }
    }
    Status st = w.create_index(t.name, idx);
    if (st.ok()) mt.def.indexes.push_back(idx);
    return verdict("create_index", st, model_ok);
  }
  if (pick < 97) {
    // Drop an index the schema does not rely on.
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < t.indexes.size(); ++i) {
      if (t.indexes[i].name.rfind("x_", 0) == 0) candidates.push_back(i);
    }
    if (candidates.empty()) return Status();
    const std::size_t i = candidates[rng.below(candidates.size())];
    Status st = w.drop_index(t.name, t.indexes[i].name);
    if (st.ok()) mt.def.indexes.erase(mt.def.indexes.begin() + static_cast<std::ptrdiff_t>(i));
    return verdict("drop_index", st, true);
  }
  const std::uint64_t v = m.version + 1;
  Status st = w.set_schema_version(v);
  if (st.ok()) m.version = v;
  return verdict("set_schema_version", st, true);
}

struct Outcome {
  bool crashed = false;
};

// Recover after a crash and settle an in-doubt commit.
void recover_and_settle(FaultVfs& vfs, Model& committed, const Model* in_flight, Rng& rng) {
  vfs.set_injector(nullptr);
  vfs.recover();
  auto st = Store::open(vfs, "d/s.db", opts());
  REQUIRE_MSG(st.ok(), "reopen after crash: " << st.status().to_string());
  auto p_old = compare_store(*st.value(), committed, rng, true);
  if (p_old.empty()) return;
  REQUIRE_MSG(in_flight != nullptr, "recovered state differs from the committed model: " << p_old[0]);
  auto p_new = compare_store(*st.value(), *in_flight, rng, true);
  REQUIRE_MSG(p_new.empty(), "in-doubt commit matches neither: " << p_old[0] << " / " << p_new[0]);
  committed = *in_flight;
}

}  // namespace

ARCHIVUM_TEST(store_model_random_operations) {
  std::uint64_t base_seed = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  if (const char* env = std::getenv("ARCHIVUM_SEED")) base_seed = std::strtoull(env, nullptr, 10);
  std::uint64_t iterations = 6;
  if (const char* env = std::getenv("ARCHIVUM_CRASH_ITERS")) iterations = std::max<std::uint64_t>(1, std::strtoull(env, nullptr, 10) / 50);
  std::printf("  base seed %llu, %llu iterations\n", static_cast<unsigned long long>(base_seed),
              static_cast<unsigned long long>(iterations));
  for (std::uint64_t it = 0; it < iterations; ++it) {
    const std::uint64_t seed = base_seed + it;
    Rng rng(seed);
    FaultConfig cfg;
    cfg.crash.persist = static_cast<CrashPolicy::Persist>(rng.below(4));
    cfg.seed = seed;
    FaultVfs vfs(cfg);
    std::ostringstream ctx;
    ctx << "seed=" << seed << " persist=" << static_cast<int>(cfg.crash.persist);
    set_context(ctx.str());

    Model committed;
    {
      auto st = Store::open(vfs, "d/s.db", opts());
      REQUIRE_OK(st.status());
      g_max_key = st.value()->max_key_bytes();
      auto w = st.value()->begin_write();
      REQUIRE_OK(w.status());
      for (const TableDef& d : {parents_def(), children_def(), tags_def()}) {
        REQUIRE_OK(w.value()->create_table(d));
        committed.tables[d.name].def = d;
      }
      REQUIRE_OK(w.value()->set_schema_version(1));
      committed.version = 1;
      REQUIRE_OK(w.value()->commit());
    }
    std::vector<std::string> disagreements;
    for (int round = 0; round < 4; ++round) {
      auto st_r = Store::open(vfs, "d/s.db", opts());
      REQUIRE_MSG(st_r.ok(), "open: " << st_r.status().to_string());
      Store& store = *st_r.value();
      {
        auto problems = compare_store(store, committed, rng, true);
        REQUIRE_MSG(problems.empty(), "after open: " << problems[0]);
      }
      bool crashed = false;
      for (int txn = 0; txn < 25 && !crashed; ++txn) {
        // A crash lands somewhere in the last third of each round.
        if (txn == 17) vfs.set_injector(std::make_shared<CrashAtStep>(vfs.op_count() + rng.below(400)));
        auto w = store.begin_write();
        if (!w.ok()) {
          REQUIRE_MSG(w.status().code() == ErrorCode::Crashed, "begin_write: " << w.status().to_string());
          crashed = true;
          recover_and_settle(vfs, committed, nullptr, rng);
          break;
        }
        Model next = committed;
        const int ops = 1 + static_cast<int>(rng.below(15));
        Status st;
        for (int i = 0; i < ops && st.ok(); ++i) st = apply_random_op(rng, *w.value(), next, disagreements);
        REQUIRE_MSG(disagreements.empty(), disagreements[0]);
        if (st.code() == ErrorCode::Crashed || st.code() == ErrorCode::IoError) {
          crashed = true;
          w.value().reset();
          recover_and_settle(vfs, committed, nullptr, rng);
          break;
        }
        REQUIRE_OK(st);
        if (rng.chance(6)) {
          // Writers see their own changes before rolling back.
          auto problems = compare(*w.value(), next, rng);
          if (vfs.crashed()) {
            crashed = true;
            w.value().reset();
            recover_and_settle(vfs, committed, nullptr, rng);
            break;
          }
          REQUIRE_MSG(problems.empty(), "inside transaction: " << problems[0]);
          w.value()->rollback();
          continue;
        }
        Status c = w.value()->commit();
        if (c.code() == ErrorCode::Crashed || c.code() == ErrorCode::IoError) {
          crashed = true;
          w.value().reset();
          recover_and_settle(vfs, committed, &next, rng);
          break;
        }
        REQUIRE_OK(c);
        committed = next;
        if (rng.chance(4)) {
          Status cp = store.checkpoint();
          if (cp.code() == ErrorCode::Crashed || cp.code() == ErrorCode::IoError) {
            crashed = true;
            recover_and_settle(vfs, committed, nullptr, rng);
            break;
          }
          REQUIRE_OK(cp);
        }
        auto problems = compare_store(store, committed, rng, txn % 5 == 0);
        if (vfs.crashed()) {
          crashed = true;
          recover_and_settle(vfs, committed, nullptr, rng);
          break;
        }
        REQUIRE_MSG(problems.empty(), "after commit: " << problems[0]);
      }
      if (!crashed) {
        vfs.crash();
        recover_and_settle(vfs, committed, nullptr, rng);
      }
      vfs.set_injector(nullptr);
    }
  }
  set_context("");
}
