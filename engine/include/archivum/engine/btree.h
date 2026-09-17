// B+tree over the pager: an ordered map from byte-string keys to byte-string
// values. Keys compare as unsigned bytes (memcmp order), so typed keys are
// encoded order-preservingly by the record layer and the tree stays
// type-agnostic. Values of any size are supported through overflow chains.
//
// Structure (docs/btree-format.md):
//   * interior nodes hold (separator key, left child) cells plus a rightmost
//     child; a child under separator K holds keys < K;
//   * leaves hold (key, value) cells and are doubly linked for range scans;
//   * the root page number never changes: a root split copies the old root
//     into a new page and rewrites the root as an interior node;
//   * a leaf that becomes empty is unlinked and freed, and an interior node
//     left with only its rightmost child collapses into it. Underfull nodes
//     are otherwise not merged (space is reused by later inserts).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "archivum/engine/db.h"

namespace archivum::engine {

// Page access the tree needs. ReadTxn and WriteTxn are adapted to these.
class PageReader {
 public:
  virtual ~PageReader() = default;
  virtual std::uint32_t page_size() const = 0;
  virtual Status read_page(PageNo page, std::span<std::byte> out) = 0;
};

class PageWriter : public PageReader {
 public:
  virtual Status write_page(PageNo page, std::span<const std::byte> data) = 0;
  virtual Result<PageNo> allocate_page() = 0;
  virtual Status free_page(PageNo page) = 0;
};

class ReadTxnPages final : public PageReader {
 public:
  explicit ReadTxnPages(ReadTxn& txn, std::uint32_t page_size) : txn_(txn), page_size_(page_size) {}
  std::uint32_t page_size() const override { return page_size_; }
  Status read_page(PageNo page, std::span<std::byte> out) override { return txn_.read_page(page, out); }

 private:
  ReadTxn& txn_;
  std::uint32_t page_size_;
};

class WriteTxnPages final : public PageWriter {
 public:
  explicit WriteTxnPages(WriteTxn& txn, std::uint32_t page_size) : txn_(txn), page_size_(page_size) {}
  std::uint32_t page_size() const override { return page_size_; }
  Status read_page(PageNo page, std::span<std::byte> out) override { return txn_.read_page(page, out); }
  Status write_page(PageNo page, std::span<const std::byte> data) override {
    return txn_.write_page(page, data);
  }
  Result<PageNo> allocate_page() override { return txn_.allocate_page(); }
  Status free_page(PageNo page) override { return txn_.free_page(page); }

 private:
  WriteTxn& txn_;
  std::uint32_t page_size_;
};

using Bytes = std::vector<std::byte>;

struct BTreeCheckReport {
  bool ok = true;
  std::vector<std::string> problems;
  std::uint64_t entries = 0;
  std::uint64_t leaf_pages = 0;
  std::uint64_t interior_pages = 0;
  std::uint64_t overflow_pages = 0;
  std::uint32_t depth = 0;
  std::vector<PageNo> pages;  // every page the tree owns, for cross-checks
};

class BTree {
 public:
  // Opens an existing tree. `writer` may be null for read-only use.
  BTree(PageReader& reader, PageWriter* writer, PageNo root);

  // Allocates an empty tree and returns its root page.
  static Result<PageNo> create(PageWriter& writer);
  // Frees every page of the tree, root included.
  static Status destroy(PageWriter& writer, PageNo root);

  PageNo root() const { return root_; }
  std::uint32_t max_key_bytes() const;
  static std::uint32_t max_key_bytes_for(std::uint32_t page_size);

  Result<std::optional<Bytes>> get(std::span<const std::byte> key);
  // Inserts or replaces.
  Status put(std::span<const std::byte> key, std::span<const std::byte> value);
  // Inserts only if absent; `inserted` reports which.
  Status insert_if_absent(std::span<const std::byte> key, std::span<const std::byte> value, bool& inserted);
  // Removes; `existed` reports whether the key was present.
  Status erase(std::span<const std::byte> key, bool& existed);

  class Cursor {
   public:
    // Positions at the first entry with key >= `key` (or the end).
    Status seek(std::span<const std::byte> key);
    Status seek_first();
    Status seek_last();
    Status next();
    Status prev();
    bool valid() const { return valid_; }
    const Bytes& key() const { return key_; }
    // Loads the value (following overflow) on demand.
    Result<Bytes> value();

   private:
    friend class BTree;
    explicit Cursor(BTree& tree) : tree_(tree) {}
    Status load(PageNo leaf, std::size_t slot);
    Status load_slot();
    BTree& tree_;
    PageNo leaf_ = 0;
    std::size_t slot_ = 0;
    Bytes page_;
    Bytes key_;
    bool valid_ = false;
  };
  Cursor cursor() { return Cursor(*this); }

  // Structural invariants: node types, ordering within and across nodes,
  // separator bounds, leaf linkage, overflow chains, page ownership.
  Result<BTreeCheckReport> check();

  // Visits every entry in key order.
  Status for_each(const std::function<Status(std::span<const std::byte>, std::span<const std::byte>)>& fn);

 private:
  struct PathEntry {
    PageNo page;
    std::size_t slot;  // for interior: index of the cell descended through, or count for right child
  };
  Status read(PageNo page, Bytes& buf);
  Status write(PageNo page, const Bytes& buf);
  Status find_leaf(std::span<const std::byte> key, std::vector<PathEntry>& path, Bytes& leaf);
  Status insert_into_leaf(std::vector<PathEntry>& path, Bytes& leaf, std::span<const std::byte> key,
                          std::span<const std::byte> value, std::size_t slot, bool replace);
  Status insert_separator(std::vector<PathEntry>& path, std::size_t depth, const Bytes& sep,
                          PageNo right_page);
  Status remove_child(std::vector<PathEntry>& path, std::size_t depth);
  Status write_overflow(std::span<const std::byte> value, PageNo& first);
  Status read_overflow(PageNo first, std::uint32_t total, Bytes& out);
  Status free_overflow(PageNo first);
  Status free_cell_overflow(const Bytes& leaf, std::size_t slot);
  Status check_node(PageNo page, std::uint32_t depth, const Bytes* lower, const Bytes* upper,
                    BTreeCheckReport& r, std::uint32_t& leaf_depth, PageNo& expected_prev,
                    std::uint64_t& entries);

  PageReader& reader_;
  PageWriter* writer_;
  PageNo root_;
  std::uint32_t page_size_;
};

}  // namespace archivum::engine
