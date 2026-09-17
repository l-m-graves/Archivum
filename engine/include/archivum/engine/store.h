// The typed layer: tables, secondary indexes, constraints and a catalog,
// all stored as b-trees in one Db. This is the programmatic API the SQL
// layer (Stage 4) and the application server build on.
//
// Layout (docs/store-format.md):
//   * the catalog is a b-tree whose root is always page 1; it holds the
//     schema version and one entry per table (columns, primary key,
//     indexes with their root pages, foreign keys, checks);
//   * each table has a primary-key b-tree: key = order-preserving encoding
//     of the primary-key columns, value = the whole row;
//   * each secondary index is a b-tree: key = encoding of the indexed
//     columns followed by the encoding of the primary key, value empty, so
//     non-unique indexes still have distinct keys and every index entry
//     names its row.
//
// Transactions: a Writer is the Db's single writer; schema changes and row
// changes in one Writer commit atomically, so a migration is one write
// transaction. Readers see the snapshot they started on, catalog included.
//
// Constraints, enforced here and nowhere else: declared types, NOT NULL,
// primary key, UNIQUE indexes (NULLs distinct), FOREIGN KEY with RESTRICT
// semantics in both directions (a child row needs its parent at insert and
// update; a parent row cannot be deleted or have its referenced key changed
// while children point at it), and simple CHECKs (column op constant, or
// column IN a list). A violation returns ErrorCode::Constraint and the
// message names the constraint.
#pragma once

#include <functional>
#include <cstdint>
#include <string_view>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "archivum/engine/btree.h"
#include "archivum/engine/db.h"
#include "archivum/engine/record.h"
#include "archivum/engine/types.h"

namespace archivum::engine {

struct IndexDef {
  std::string name;
  std::vector<std::string> columns;
  bool unique = false;
  PageNo root = 0;  // assigned by the engine
};

enum class CheckOp : std::uint8_t { Eq = 1, Ne, Lt, Le, Gt, Ge, In };

struct CheckDef {
  std::string name;
  std::string column;
  CheckOp op = CheckOp::Eq;
  std::vector<Value> operands;  // one, or the IN list
};

struct ForeignKeyDef {
  std::string name;
  std::vector<std::string> columns;
  std::string ref_table;
  std::vector<std::string> ref_columns;  // the parent's primary key or a unique index
};

struct TableDef {
  std::string name;
  std::vector<ColumnDef> columns;
  std::vector<std::string> primary_key;
  std::vector<IndexDef> indexes;
  std::vector<ForeignKeyDef> foreign_keys;
  std::vector<CheckDef> checks;
  PageNo root = 0;  // assigned by the engine

  // -1 when absent.
  int column_index(std::string_view name) const;
  const IndexDef* index(std::string_view name) const;
};

struct Catalog {
  std::uint64_t schema_version = 0;
  std::map<std::string, TableDef> tables;
  const TableDef* table(std::string_view name) const;
};

// A bound on a scan: the leading key columns (a prefix of the index's
// columns is allowed) and whether the bound itself is included.
struct Bound {
  Row values;
  bool inclusive = true;
};

class Store;

struct StoreCheckReport {
  bool ok = true;
  std::vector<std::string> problems;
  CheckReport pager;
  std::uint64_t tables = 0;
  std::uint64_t rows = 0;
  std::uint64_t index_entries = 0;
  std::uint64_t pages_owned = 0;  // b-tree pages of every table, index and the catalog
};

class Reader {
 public:
  virtual ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  const Catalog& catalog() const { return *catalog_; }
  Result<std::optional<Row>> get(const std::string& table, const Row& primary_key);
  // Scans `table` in key order of its primary key (`index` empty) or of a
  // secondary index, between the bounds, forward or in reverse. The
  // callback returns false to stop early.
  Status scan(const std::string& table, const std::string& index, const std::optional<Bound>& lower,
              const std::optional<Bound>& upper, bool reverse, const std::function<bool(const Row&)>& fn);
  Result<std::vector<Row>> scan_all(const std::string& table, const std::string& index = "",
                                    const std::optional<Bound>& lower = std::nullopt,
                                    const std::optional<Bound>& upper = std::nullopt, bool reverse = false);
  Result<std::uint64_t> count(const std::string& table);

 protected:
  friend class Store;
  Reader(Store& store, std::shared_ptr<const Catalog> catalog);
  Status check_into(StoreCheckReport& report);
  Status scan_raw(const TableDef& t, const IndexDef* idx, const std::optional<Bound>& lower,
                  const std::optional<Bound>& upper, bool reverse,
                  const std::function<Result<bool>(const Bytes& key)>& fn);
  Result<bool> index_prefix_exists(const TableDef& t, const IndexDef& idx, const Row& values);
  Result<bool> prefix_exists(PageNo root, const Row& values);
  Result<std::optional<Row>> get_by_key(const TableDef& t, const Bytes& pk_key);
  Status check_parents(const TableDef& t, const Row& row, const Row* old_row);
  Status check_no_children(const TableDef& t, const Row& row, const Row* new_row);
  BTree tree(PageNo root) { return BTree(*pages_, writer_, root); }

  Store& store_;
  std::unique_ptr<ReadTxn> rtxn_;
  std::unique_ptr<ReadTxnPages> rpages_;
  PageReader* pages_ = nullptr;
  PageWriter* writer_ = nullptr;
  std::shared_ptr<const Catalog> catalog_;
};

class Writer : public Reader {
 public:
  ~Writer() override;

  // Rows: `row` has one value per column in declaration order. update
  // replaces the row with the same primary key (NotFound if absent);
  // remove takes the primary key values.
  Status insert(const std::string& table, const Row& row);
  Status update(const std::string& table, const Row& row);
  Status remove(const std::string& table, const Row& primary_key);

  // Schema. Root pages in the definitions are assigned by the engine.
  Status create_table(TableDef def);
  Status drop_table(const std::string& table);
  Status create_index(const std::string& table, IndexDef def);
  Status drop_index(const std::string& table, const std::string& index);
  Status set_schema_version(std::uint64_t version);

  // Durable when ok is returned. Rolls back if neither is called.
  Status commit();
  void rollback();

  // Re-checks every foreign key of `table` against the current state; used
  // by restore, which inserts without per-row checks, and by check().
  Status verify_foreign_keys(const std::string& table);

 private:
  friend class Store;
  Writer(Store& store, std::unique_ptr<WriteTxn> txn, std::shared_ptr<const Catalog> catalog);
  Status put_index_entries(const TableDef& t, const Row& row, const Bytes& pk_key, const Row* old_row);
  Status erase_index_entries(const TableDef& t, const Row& row, const Bytes& pk_key);
  Status insert_unchecked(const TableDef& t, const Row& row, bool check_fks);
  Status save_table(const TableDef& t);
  Status save_version();
  Catalog& mutable_catalog();

  std::unique_ptr<WriteTxn> txn_;
  std::unique_ptr<WriteTxnPages> wpages_;
  std::shared_ptr<Catalog> edited_;  // the catalog copy this writer modifies
  bool schema_changed_ = false;
  bool finished_ = false;
};

struct TableDump {
  TableDef def;  // roots zeroed
  std::vector<Row> rows;
};
struct Dump {
  std::uint64_t schema_version = 0;
  std::vector<TableDump> tables;
};

class Store {
 public:
  static Result<std::unique_ptr<Store>> open(Vfs& vfs, std::string path, DbOptions options = {});
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  Result<std::unique_ptr<Reader>> begin_read();
  // Blocks while another Writer is active.
  Result<std::unique_ptr<Writer>> begin_write();

  Status checkpoint() { return db_->checkpoint(); }
  Db& db() { return *db_; }
  // The longest encoded primary key, or index key plus primary key, a
  // table of this store can hold; a longer one is refused as InvalidArgument.
  std::uint32_t max_key_bytes() const { return BTree::max_key_bytes_for(db_->page_size()); }

  // Every invariant: pager checks, each b-tree's structure, every index
  // entry has its row and every row has its index entries, unique indexes
  // and primary keys are unique, foreign keys resolve, checks and NOT NULL
  // hold, and the catalog's, tables', indexes' and free list's pages
  // partition the file with no page owned twice or by nobody.
  Result<StoreCheckReport> check();

  // Logical dump of the whole store and its inverse into an empty store.
  Result<Dump> dump();
  Status restore(const Dump& dump);

  Status close();

 private:
  friend class Reader;
  friend class Writer;
  explicit Store(std::unique_ptr<Db> db);
  Result<std::shared_ptr<const Catalog>> catalog_for(std::uint64_t change_counter, PageReader& pages);
  void catalog_committed(std::uint64_t change_counter, std::shared_ptr<const Catalog> catalog);

  std::unique_ptr<Db> db_;
  std::mutex catalog_mu_;
  std::uint64_t catalog_change_counter_ = 0;
  std::shared_ptr<const Catalog> catalog_;
};

// Catalog encoding, exposed for tests.
Row encode_table_def(const TableDef& t);
Result<TableDef> decode_table_def(const Row& row);
Status load_catalog(PageReader& pages, Catalog& out);

constexpr PageNo kCatalogRoot = 1;

}  // namespace archivum::engine
