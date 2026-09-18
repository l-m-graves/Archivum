#include "archivum/engine/migrate.h"

#include <unordered_set>

namespace archivum::engine {
namespace {

TableDef migrations_table() {
  TableDef t;
  t.name = kMigrationsTable;
  t.columns = {{"module", ColumnType::Text, false, 0},
               {"version", ColumnType::Integer, false, 0},
               {"name", ColumnType::Text, false, 0},
               {"applied_at", ColumnType::Timestamp, false, 0}};
  t.primary_key = {"module", "version"};
  t.checks = {{"name_nonempty", "name", CheckOp::Ne, {Value::text("")}},
              {"module_nonempty", "module", CheckOp::Ne, {Value::text("")}}};
  return t;
}

}  // namespace

Status validate_migrations(const std::vector<Migration>& migrations) {
  std::unordered_set<std::string> names;
  for (std::size_t i = 0; i < migrations.size(); ++i) {
    const Migration& m = migrations[i];
    if (m.version != i + 1) {
      return Status::invalid_argument("migration " + m.name + " has version " + std::to_string(m.version) +
                                      ", expected " + std::to_string(i + 1));
    }
    if (m.name.empty()) return Status::invalid_argument("migration " + std::to_string(m.version) + " has no name");
    if (!names.insert(m.name).second) return Status::invalid_argument("migration name repeated: " + m.name);
    if (!m.apply) return Status::invalid_argument("migration " + m.name + " has no apply function");
  }
  return Status();
}

Result<std::vector<std::pair<std::uint64_t, std::string>>> applied_migrations(Reader& reader, const std::string& module) {
  std::vector<std::pair<std::uint64_t, std::string>> out;
  if (reader.catalog().table(kMigrationsTable) == nullptr) return out;
  auto rows = reader.scan_all(kMigrationsTable, "", Bound{{Value::text(module)}}, Bound{{Value::text(module)}});
  if (!rows.ok()) return rows.status();
  for (const Row& r : rows.value()) out.emplace_back(static_cast<std::uint64_t>(r[1].as_int64()), r[2].as_text());
  return out;
}

Result<MigrationReport> migrate(Store& store, const std::string& module, const std::vector<Migration>& migrations) {
  if (module.empty()) return Status::invalid_argument("migration module needs a name");
  if (Status s = validate_migrations(migrations); !s.ok()) return s;
  MigrationReport rep;
  {
    auto r = store.begin_read();
    if (!r.ok()) return r.status();
    auto applied = applied_migrations(*r.value(), module);
    if (!applied.ok()) return applied.status();
    rep.from_version = applied.value().size();
    rep.to_version = rep.from_version;
    if (applied.value().size() > migrations.size()) {
      return Status::unsupported("database has " + std::to_string(applied.value().size()) + " migrations of " + module +
                                 ", newer than this binary's " + std::to_string(migrations.size()));
    }
    for (std::size_t i = 0; i < applied.value().size(); ++i) {
      if (applied.value()[i].first != i + 1 || applied.value()[i].second != migrations[i].name) {
        return Status::corrupt("migration " + module + " " + std::to_string(i + 1) + " recorded as '" +
                               applied.value()[i].second + "', this binary has '" + migrations[i].name + "'");
      }
    }
  }
  for (std::size_t i = rep.from_version; i < migrations.size(); ++i) {
    const Migration& m = migrations[i];
    auto w = store.begin_write();
    if (!w.ok()) return w.status();
    Writer& writer = *w.value();
    if (writer.catalog().table(kMigrationsTable) == nullptr) {
      if (Status s = writer.create_table(migrations_table()); !s.ok()) return s;
    }
    // Another writer may have applied this step between our read and now.
    auto now_applied = applied_migrations(writer, module);
    if (!now_applied.ok()) return now_applied.status();
    if (now_applied.value().size() != m.version - 1) return Status::busy("schema changed underneath the migration");
    if (Status s = m.apply(writer); !s.ok()) return Status(s.code(), "migration " + module + " " + m.name + ": " + s.message());
    Row row = {Value::text(module), Value::integer(static_cast<std::int64_t>(m.version)), Value::text(m.name),
               Value::timestamp(store.db().now_us())};
    if (Status s = writer.insert(kMigrationsTable, row); !s.ok()) return s;
    if (Status s = writer.set_schema_version(writer.catalog().schema_version + 1); !s.ok()) return s;
    if (Status s = writer.commit(); !s.ok()) return s;
    rep.applied.push_back(m.name);
    rep.to_version = m.version;
  }
  return rep;
}

}  // namespace archivum::engine
