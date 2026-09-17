#include "archivum/engine/migrate.h"

#include <unordered_set>

namespace archivum::engine {
namespace {

TableDef migrations_table() {
  TableDef t;
  t.name = kMigrationsTable;
  t.columns = {{"version", ColumnType::Integer, false, 0},
               {"name", ColumnType::Text, false, 0},
               {"applied_at", ColumnType::Timestamp, false, 0}};
  t.primary_key = {"version"};
  t.checks = {{"name_nonempty", "name", CheckOp::Ne, {Value::text("")}}};
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

Result<MigrationReport> migrate(Store& store, const std::vector<Migration>& migrations) {
  if (Status s = validate_migrations(migrations); !s.ok()) return s;
  MigrationReport rep;
  {
    auto r = store.begin_read();
    if (!r.ok()) return r.status();
    const Catalog& cat = r.value()->catalog();
    rep.from_version = cat.schema_version;
    rep.to_version = cat.schema_version;
    if (cat.schema_version > migrations.size()) {
      return Status::unsupported("database schema version " + std::to_string(cat.schema_version) +
                                 " is newer than this binary's " + std::to_string(migrations.size()));
    }
    if (cat.schema_version > 0) {
      if (cat.table(kMigrationsTable) == nullptr) {
        return Status::corrupt("schema version " + std::to_string(cat.schema_version) + " but no " +
                               std::string(kMigrationsTable) + " table");
      }
      auto rows = r.value()->scan_all(kMigrationsTable);
      if (!rows.ok()) return rows.status();
      if (rows.value().size() != cat.schema_version) {
        return Status::corrupt(std::string(kMigrationsTable) + " has " + std::to_string(rows.value().size()) +
                               " rows for schema version " + std::to_string(cat.schema_version));
      }
      for (std::size_t i = 0; i < rows.value().size(); ++i) {
        const Row& row = rows.value()[i];
        if (row[0].as_int64() != static_cast<std::int64_t>(i + 1) || row[1].as_text() != migrations[i].name) {
          return Status::corrupt("migration " + std::to_string(i + 1) + " recorded as '" + row[1].to_string() +
                                 "', this binary has '" + migrations[i].name + "'");
        }
      }
    }
  }
  for (std::size_t i = rep.from_version; i < migrations.size(); ++i) {
    const Migration& m = migrations[i];
    auto w = store.begin_write();
    if (!w.ok()) return w.status();
    Writer& writer = *w.value();
    if (writer.catalog().schema_version != m.version - 1) {
      return Status::busy("schema changed underneath the migration");
    }
    if (writer.catalog().table(kMigrationsTable) == nullptr) {
      if (Status s = writer.create_table(migrations_table()); !s.ok()) return s;
    }
    if (Status s = m.apply(writer); !s.ok()) return Status(s.code(), "migration " + m.name + ": " + s.message());
    Row row = {Value::integer(static_cast<std::int64_t>(m.version)), Value::text(m.name),
               Value::timestamp(store.db().now_us())};
    if (Status s = writer.insert(kMigrationsTable, row); !s.ok()) return s;
    if (Status s = writer.set_schema_version(m.version); !s.ok()) return s;
    if (Status s = writer.commit(); !s.ok()) return s;
    rep.applied.push_back(m.name);
    rep.to_version = m.version;
  }
  return rep;
}

}  // namespace archivum::engine
