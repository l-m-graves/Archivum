#include "archivum/core/ids.h"

namespace archivum::core {

Result<std::int64_t> next_id(engine::Reader& reader, const std::string& table) {
  const engine::TableDef* t = reader.catalog().table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  if (t->primary_key.size() != 1 || t->columns[static_cast<std::size_t>(t->column_index(t->primary_key[0]))].type !=
                                        engine::ColumnType::Integer) {
    return Status::invalid_argument("table " + table + " has no single integer key");
  }
  std::int64_t last = 0;
  Status s = reader.scan(table, "", std::nullopt, std::nullopt, true, [&](const engine::Row& r) {
    last = r[static_cast<std::size_t>(t->column_index(t->primary_key[0]))].as_int64();
    return false;
  });
  if (!s.ok()) return s;
  return last + 1;
}

}  // namespace archivum::core
