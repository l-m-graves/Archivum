#include "archivum/engine/btree.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace archivum::engine {
namespace {

constexpr std::uint8_t kLeaf = 1;
constexpr std::uint8_t kInterior = 2;
constexpr std::size_t kHdr = 24;
constexpr std::size_t kLeafCellHdr = 7;      // u16 key_len, u8 flags, u32 value_len
constexpr std::size_t kInteriorCellHdr = 10;  // u16 key_len, u64 child
constexpr std::uint8_t kFlagOverflow = 1;
constexpr std::size_t kOverflowHdr = 8;  // u64 next

void put_u16(std::byte* p, std::uint16_t v) {
  p[0] = static_cast<std::byte>(v & 0xFFu);
  p[1] = static_cast<std::byte>((v >> 8) & 0xFFu);
}
std::uint16_t get_u16(const std::byte* p) {
  return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(p[0]) |
                                    (std::to_integer<std::uint16_t>(p[1]) << 8));
}

int compare(std::span<const std::byte> a, std::span<const std::byte> b) {
  const std::size_t n = std::min(a.size(), b.size());
  const int c = n == 0 ? 0 : std::memcmp(a.data(), b.data(), n);
  if (c != 0) return c;
  if (a.size() < b.size()) return -1;
  return a.size() > b.size() ? 1 : 0;
}

// A view over one node's body (the page minus its trailer).
struct Node {
  std::byte* p;
  std::size_t usable;

  Node(Bytes& page, std::uint32_t page_size) : p(page.data()), usable(page_size - kPageTrailerBytes) {}

  static void init(Bytes& page, std::uint32_t page_size, std::uint8_t type) {
    std::fill(page.begin(), page.end(), std::byte{0});
    Node n(page, page_size);
    n.p[0] = static_cast<std::byte>(type);
    n.set_count(0);
    n.set_content_start(static_cast<std::uint16_t>(n.usable));
    n.set_frag(0);
    n.set_right(0);
    n.set_prev(0);
  }

  std::uint8_t type() const { return std::to_integer<std::uint8_t>(p[0]); }
  bool is_leaf() const { return type() == kLeaf; }
  std::uint16_t count() const { return get_u16(p + 2); }
  void set_count(std::uint16_t v) { put_u16(p + 2, v); }
  std::uint16_t content_start() const { return get_u16(p + 4); }
  void set_content_start(std::uint16_t v) { put_u16(p + 4, v); }
  std::uint16_t frag() const { return get_u16(p + 6); }
  void set_frag(std::uint16_t v) { put_u16(p + 6, v); }
  PageNo right() const { return get_u64(p + 8); }  // interior: rightmost child; leaf: next leaf
  void set_right(PageNo v) { put_u64(p + 8, v); }
  PageNo prev() const { return get_u64(p + 16); }
  void set_prev(PageNo v) { put_u64(p + 16, v); }

  std::uint16_t slot(std::size_t i) const { return get_u16(p + kHdr + 2 * i); }
  void set_slot(std::size_t i, std::uint16_t off) { put_u16(p + kHdr + 2 * i, off); }
  std::byte* cell(std::size_t i) { return p + slot(i); }
  const std::byte* cell(std::size_t i) const { return p + slot(i); }

  std::uint16_t key_len(std::size_t i) const { return get_u16(cell(i)); }
  std::span<const std::byte> key(std::size_t i) const {
    const std::byte* c = cell(i);
    return {c + (is_leaf() ? kLeafCellHdr : kInteriorCellHdr), key_len(i)};
  }
  // Leaf accessors
  std::uint8_t flags(std::size_t i) const { return std::to_integer<std::uint8_t>(cell(i)[2]); }
  bool overflowed(std::size_t i) const { return (flags(i) & kFlagOverflow) != 0; }
  std::uint32_t value_len(std::size_t i) const { return get_u32(cell(i) + 3); }
  std::span<const std::byte> inline_value(std::size_t i) const {
    const std::byte* c = cell(i);
    return {c + kLeafCellHdr + key_len(i), value_len(i)};
  }
  PageNo overflow_page(std::size_t i) const { return get_u64(cell(i) + kLeafCellHdr + key_len(i)); }
  // Interior accessors
  PageNo child(std::size_t i) const { return get_u64(cell(i) + 2); }
  void set_child(std::size_t i, PageNo c) { put_u64(cell(i) + 2, c); }

  std::size_t cell_size(std::size_t i) const {
    if (is_leaf()) return kLeafCellHdr + key_len(i) + (overflowed(i) ? 8 : value_len(i));
    return kInteriorCellHdr + key_len(i);
  }
  std::size_t free_space() const { return content_start() - (kHdr + 2 * count()); }

  // Returns the slot of the first key >= `k` and whether it is an exact match.
  std::size_t lower_bound(std::span<const std::byte> k, bool* exact) const {
    std::size_t lo = 0, hi = count();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      if (compare(key(mid), k) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (exact != nullptr) *exact = lo < count() && compare(key(lo), k) == 0;
    return lo;
  }

  bool fits(std::size_t cell_bytes) const { return free_space() >= cell_bytes + 2; }
  bool fits_after_defrag(std::size_t cell_bytes) const { return free_space() + frag() >= cell_bytes + 2; }

  void insert_cell(std::size_t at, std::span<const std::byte> cell_bytes) {
    const std::uint16_t n = count();
    const std::uint16_t off = static_cast<std::uint16_t>(content_start() - cell_bytes.size());
    std::memcpy(p + off, cell_bytes.data(), cell_bytes.size());
    std::memmove(p + kHdr + 2 * (at + 1), p + kHdr + 2 * at, 2 * (n - at));
    set_slot(at, off);
    set_count(static_cast<std::uint16_t>(n + 1));
    set_content_start(off);
  }
  void remove_cell(std::size_t at) {
    const std::uint16_t n = count();
    const std::size_t size = cell_size(at);
    set_frag(static_cast<std::uint16_t>(frag() + size));
    std::memmove(p + kHdr + 2 * at, p + kHdr + 2 * (at + 1), 2 * (n - at - 1));
    set_count(static_cast<std::uint16_t>(n - 1));
  }
  // Repacks cells at the end of the body, preserving slot order.
  void defragment(Bytes& scratch) {
    const std::uint16_t n = count();
    scratch.assign(usable, std::byte{0});
    std::size_t off = usable;
    std::vector<std::uint16_t> offs(n);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t size = cell_size(i);
      off -= size;
      std::memcpy(scratch.data() + off, cell(i), size);
      offs[i] = static_cast<std::uint16_t>(off);
    }
    std::memcpy(p + off, scratch.data() + off, usable - off);
    for (std::size_t i = 0; i < n; ++i) set_slot(i, offs[i]);
    set_content_start(static_cast<std::uint16_t>(off));
    set_frag(0);
  }
};

Bytes build_leaf_cell(std::span<const std::byte> key, std::span<const std::byte> inline_value,
                      bool overflowed, std::uint32_t total_len, PageNo overflow_first) {
  Bytes c(kLeafCellHdr + key.size() + (overflowed ? 8 : inline_value.size()));
  put_u16(c.data(), static_cast<std::uint16_t>(key.size()));
  c[2] = static_cast<std::byte>(overflowed ? kFlagOverflow : 0);
  put_u32(c.data() + 3, total_len);
  std::memcpy(c.data() + kLeafCellHdr, key.data(), key.size());
  if (overflowed) {
    put_u64(c.data() + kLeafCellHdr + key.size(), overflow_first);
  } else if (!inline_value.empty()) {
    std::memcpy(c.data() + kLeafCellHdr + key.size(), inline_value.data(), inline_value.size());
  }
  return c;
}

Bytes build_interior_cell(std::span<const std::byte> key, PageNo child) {
  Bytes c(kInteriorCellHdr + key.size());
  put_u16(c.data(), static_cast<std::uint16_t>(key.size()));
  put_u64(c.data() + 2, child);
  std::memcpy(c.data() + kInteriorCellHdr, key.data(), key.size());
  return c;
}

std::size_t max_cell_bytes(std::uint32_t page_size) {
  return (page_size - kPageTrailerBytes - kHdr) / 4 - 2;
}

}  // namespace

// ---------------------------------------------------------------------------

BTree::BTree(PageReader& reader, PageWriter* writer, PageNo root)
    : reader_(reader), writer_(writer), root_(root), page_size_(reader.page_size()) {}

std::uint32_t BTree::max_key_bytes_for(std::uint32_t page_size) {
  return static_cast<std::uint32_t>(max_cell_bytes(page_size) - kLeafCellHdr - 8);
}

std::uint32_t BTree::max_key_bytes() const {
  return max_key_bytes_for(page_size_);
}

Result<PageNo> BTree::create(PageWriter& writer) {
  auto page = writer.allocate_page();
  if (!page.ok()) return page.status();
  Bytes buf(writer.page_size());
  Node::init(buf, writer.page_size(), kLeaf);
  if (Status s = writer.write_page(page.value(), buf); !s.ok()) return s;
  return page.value();
}

Status BTree::read(PageNo page, Bytes& buf) {
  buf.resize(page_size_);
  return reader_.read_page(page, buf);
}

Status BTree::write(PageNo page, const Bytes& buf) {
  if (writer_ == nullptr) return Status::invalid_argument("b-tree opened read-only");
  return writer_->write_page(page, buf);
}

Status BTree::destroy(PageWriter& writer, PageNo root) {
  BTree t(writer, &writer, root);
  auto report = t.check();
  if (!report.ok()) return report.status();
  for (PageNo p : report.value().pages) {
    if (Status s = writer.free_page(p); !s.ok()) return s;
  }
  return Status();
}

// --- overflow ---------------------------------------------------------------

Status BTree::write_overflow(std::span<const std::byte> value, PageNo& first) {
  const std::size_t cap = page_size_ - kPageTrailerBytes - kOverflowHdr;
  std::vector<PageNo> pages;
  const std::size_t n = (value.size() + cap - 1) / cap;
  for (std::size_t i = 0; i < n; ++i) {
    auto p = writer_->allocate_page();
    if (!p.ok()) return p.status();
    pages.push_back(p.value());
  }
  Bytes buf(page_size_);
  for (std::size_t i = 0; i < n; ++i) {
    std::fill(buf.begin(), buf.end(), std::byte{0});
    put_u64(buf.data(), i + 1 < n ? pages[i + 1] : 0);
    const std::size_t off = i * cap;
    const std::size_t len = std::min(cap, value.size() - off);
    std::memcpy(buf.data() + kOverflowHdr, value.data() + off, len);
    if (Status s = writer_->write_page(pages[i], buf); !s.ok()) return s;
  }
  first = pages.empty() ? 0 : pages[0];
  return Status();
}

Status BTree::read_overflow(PageNo first, std::uint32_t total, Bytes& out) {
  const std::size_t cap = page_size_ - kPageTrailerBytes - kOverflowHdr;
  out.clear();
  out.reserve(total);
  Bytes buf;
  PageNo page = first;
  std::size_t remaining = total;
  std::size_t hops = 0;
  while (remaining > 0) {
    if (page == 0) return Status::corrupt("overflow chain ends early");
    if (++hops > total / cap + 2) return Status::corrupt("overflow chain too long");
    if (Status s = read(page, buf); !s.ok()) return s;
    const std::size_t len = std::min(cap, remaining);
    out.insert(out.end(), buf.begin() + kOverflowHdr, buf.begin() + static_cast<std::ptrdiff_t>(kOverflowHdr + len));
    remaining -= len;
    page = get_u64(buf.data());
  }
  return Status();
}

Status BTree::free_overflow(PageNo first) {
  Bytes buf;
  PageNo page = first;
  std::size_t hops = 0;
  while (page != 0) {
    if (++hops > 1u << 24) return Status::corrupt("overflow chain cycle");
    if (Status s = read(page, buf); !s.ok()) return s;
    const PageNo next = get_u64(buf.data());
    if (Status s = writer_->free_page(page); !s.ok()) return s;
    page = next;
  }
  return Status();
}

Status BTree::free_cell_overflow(const Bytes& leaf, std::size_t slot) {
  Bytes copy = leaf;
  Node n(copy, page_size_);
  if (!n.overflowed(slot)) return Status();
  return free_overflow(n.overflow_page(slot));
}

// --- navigation -------------------------------------------------------------

Status BTree::find_leaf(std::span<const std::byte> key, std::vector<PathEntry>& path, Bytes& leaf) {
  path.clear();
  PageNo page = root_;
  for (std::uint32_t depth = 0; depth < 64; ++depth) {
    if (Status s = read(page, leaf); !s.ok()) return s;
    Node n(leaf, page_size_);
    if (n.is_leaf()) {
      path.push_back({page, n.lower_bound(key, nullptr)});
      return Status();
    }
    if (n.type() != kInterior) return Status::corrupt("b-tree page " + std::to_string(page) + " has bad type");
    // First separator strictly greater than the key: descend into its left child.
    std::size_t lo = 0, hi = n.count();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      if (compare(n.key(mid), key) <= 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    path.push_back({page, lo});
    page = lo < n.count() ? n.child(lo) : n.right();
    if (page == 0) return Status::corrupt("b-tree interior page has null child");
  }
  return Status::corrupt("b-tree too deep");
}

Result<std::optional<Bytes>> BTree::get(std::span<const std::byte> key) {
  std::vector<PathEntry> path;
  Bytes leaf;
  if (Status s = find_leaf(key, path, leaf); !s.ok()) return s;
  Node n(leaf, page_size_);
  const std::size_t slot = path.back().slot;
  if (slot >= n.count() || compare(n.key(slot), key) != 0) return std::optional<Bytes>{};
  Bytes value;
  if (n.overflowed(slot)) {
    if (Status s = read_overflow(n.overflow_page(slot), n.value_len(slot), value); !s.ok()) return s;
  } else {
    const auto v = n.inline_value(slot);
    value.assign(v.begin(), v.end());
  }
  return std::optional<Bytes>(std::move(value));
}

// --- insert -----------------------------------------------------------------

Status BTree::put(std::span<const std::byte> key, std::span<const std::byte> value) {
  if (writer_ == nullptr) return Status::invalid_argument("b-tree opened read-only");
  if (key.empty() || key.size() > max_key_bytes()) {
    return Status::invalid_argument("b-tree key length " + std::to_string(key.size()) + " out of range");
  }
  std::vector<PathEntry> path;
  Bytes leaf;
  if (Status s = find_leaf(key, path, leaf); !s.ok()) return s;
  Node n(leaf, page_size_);
  const std::size_t slot = path.back().slot;
  const bool exists = slot < n.count() && compare(n.key(slot), key) == 0;
  return insert_into_leaf(path, leaf, key, value, slot, exists);
}

Status BTree::insert_if_absent(std::span<const std::byte> key, std::span<const std::byte> value,
                               bool& inserted) {
  inserted = false;
  if (writer_ == nullptr) return Status::invalid_argument("b-tree opened read-only");
  if (key.empty() || key.size() > max_key_bytes()) {
    return Status::invalid_argument("b-tree key length " + std::to_string(key.size()) + " out of range");
  }
  std::vector<PathEntry> path;
  Bytes leaf;
  if (Status s = find_leaf(key, path, leaf); !s.ok()) return s;
  Node n(leaf, page_size_);
  const std::size_t slot = path.back().slot;
  if (slot < n.count() && compare(n.key(slot), key) == 0) return Status();
  inserted = true;
  return insert_into_leaf(path, leaf, key, value, slot, false);
}

Status BTree::insert_into_leaf(std::vector<PathEntry>& path, Bytes& leaf, std::span<const std::byte> key,
                               std::span<const std::byte> value, std::size_t slot, bool replace) {
  Node n(leaf, page_size_);
  if (replace) {
    if (Status s = free_cell_overflow(leaf, slot); !s.ok()) return s;
    n.remove_cell(slot);
  }
  // Build the cell, spilling the value to an overflow chain if needed.
  const std::size_t inline_size = kLeafCellHdr + key.size() + value.size();
  Bytes cell;
  if (inline_size > max_cell_bytes(page_size_)) {
    PageNo first = 0;
    if (Status s = write_overflow(value, first); !s.ok()) return s;
    cell = build_leaf_cell(key, {}, true, static_cast<std::uint32_t>(value.size()), first);
  } else {
    cell = build_leaf_cell(key, value, false, static_cast<std::uint32_t>(value.size()), 0);
  }
  const PageNo leaf_page = path.back().page;
  if (n.fits(cell.size())) {
    n.insert_cell(slot, cell);
    return write(leaf_page, leaf);
  }
  Bytes scratch;
  if (n.fits_after_defrag(cell.size())) {
    n.defragment(scratch);
    n.insert_cell(slot, cell);
    return write(leaf_page, leaf);
  }

  // Split. Gather every cell (with the new one in place), then divide by
  // cumulative size so both halves are roughly half full.
  std::vector<Bytes> cells;
  cells.reserve(n.count() + 1);
  for (std::size_t i = 0; i < n.count(); ++i) {
    if (i == slot) cells.push_back(cell);
    cells.emplace_back(n.cell(i), n.cell(i) + n.cell_size(i));
  }
  if (slot == n.count()) cells.push_back(cell);
  std::size_t total = 0;
  for (const auto& c : cells) total += c.size() + 2;
  std::size_t split = 0, acc = 0;
  while (split < cells.size() - 1 && acc + cells[split].size() + 2 < total / 2) {
    acc += cells[split].size() + 2;
    ++split;
  }
  if (split == 0) split = 1;  // at least one cell on each side

  const PageNo old_next = n.right();
  const PageNo old_prev = n.prev();
  auto right_r = writer_->allocate_page();
  if (!right_r.ok()) return right_r.status();
  const PageNo right_page = right_r.value();
  const bool is_root = path.size() == 1;
  PageNo left_page = leaf_page;
  if (is_root) {
    // The root must keep its page number: the left half moves to a new page.
    auto lp = writer_->allocate_page();
    if (!lp.ok()) return lp.status();
    left_page = lp.value();
  }

  Bytes left(page_size_), right(page_size_);
  Node::init(left, page_size_, kLeaf);
  Node::init(right, page_size_, kLeaf);
  Node ln(left, page_size_), rn(right, page_size_);
  for (std::size_t i = 0; i < split; ++i) ln.insert_cell(i, cells[i]);
  for (std::size_t i = split; i < cells.size(); ++i) rn.insert_cell(i - split, cells[i]);
  ln.set_prev(old_prev);
  ln.set_right(right_page);
  rn.set_prev(left_page);
  rn.set_right(old_next);
  if (Status s = write(left_page, left); !s.ok()) return s;
  if (Status s = write(right_page, right); !s.ok()) return s;
  if (old_next != 0) {
    Bytes nb;
    if (Status s = read(old_next, nb); !s.ok()) return s;
    Node(nb, page_size_).set_prev(right_page);
    if (Status s = write(old_next, nb); !s.ok()) return s;
  }
  Bytes sep(rn.key(0).begin(), rn.key(0).end());
  if (is_root) {
    Bytes rootbuf(page_size_);
    Node::init(rootbuf, page_size_, kInterior);
    Node r(rootbuf, page_size_);
    r.insert_cell(0, build_interior_cell(sep, left_page));
    r.set_right(right_page);
    return write(root_, rootbuf);
  }
  return insert_separator(path, path.size() - 2, sep, right_page);
}

Status BTree::insert_separator(std::vector<PathEntry>& path, std::size_t depth, const Bytes& sep,
                               PageNo right_page) {
  const PageNo page = path[depth].page;
  const std::size_t slot = path[depth].slot;
  Bytes buf;
  if (Status s = read(page, buf); !s.ok()) return s;
  Node n(buf, page_size_);
  // The child at `slot` split: it keeps keys < sep, `right_page` takes the
  // rest. Insert (sep, child) at `slot` and hand the child's old position to
  // right_page.
  PageNo child = slot < n.count() ? n.child(slot) : n.right();
  if (slot < n.count()) {
    n.set_child(slot, right_page);
  } else {
    n.set_right(right_page);
  }
  Bytes cell = build_interior_cell(sep, child);
  if (n.fits(cell.size())) {
    n.insert_cell(slot, cell);
    return write(page, buf);
  }
  Bytes scratch;
  if (n.fits_after_defrag(cell.size())) {
    n.defragment(scratch);
    n.insert_cell(slot, cell);
    return write(page, buf);
  }

  // Interior split: cells c0..c(n-1) with the new one in place, plus the
  // rightmost child R. Left keeps c0..c(m-1) with right child c(m).child;
  // c(m).key is promoted; right keeps c(m+1).. with right child R.
  struct C {
    Bytes key;
    PageNo child;
  };
  std::vector<C> cells;
  for (std::size_t i = 0; i < n.count(); ++i) {
    if (i == slot) cells.push_back({sep, child});
    cells.push_back({Bytes(n.key(i).begin(), n.key(i).end()), n.child(i)});
  }
  if (slot == n.count()) cells.push_back({sep, child});
  const PageNo R = n.right();
  const std::size_t m = cells.size() / 2;

  auto right_r = writer_->allocate_page();
  if (!right_r.ok()) return right_r.status();
  const PageNo new_right = right_r.value();
  const bool is_root = depth == 0;
  PageNo left_page = page;
  if (is_root) {
    auto lp = writer_->allocate_page();
    if (!lp.ok()) return lp.status();
    left_page = lp.value();
  }
  Bytes left(page_size_), right(page_size_);
  Node::init(left, page_size_, kInterior);
  Node::init(right, page_size_, kInterior);
  Node ln(left, page_size_), rn(right, page_size_);
  for (std::size_t i = 0; i < m; ++i) ln.insert_cell(i, build_interior_cell(cells[i].key, cells[i].child));
  ln.set_right(cells[m].child);
  for (std::size_t i = m + 1; i < cells.size(); ++i) {
    rn.insert_cell(i - m - 1, build_interior_cell(cells[i].key, cells[i].child));
  }
  rn.set_right(R);
  if (Status s = write(left_page, left); !s.ok()) return s;
  if (Status s = write(new_right, right); !s.ok()) return s;
  const Bytes promoted = cells[m].key;
  if (is_root) {
    Bytes rootbuf(page_size_);
    Node::init(rootbuf, page_size_, kInterior);
    Node r(rootbuf, page_size_);
    r.insert_cell(0, build_interior_cell(promoted, left_page));
    r.set_right(new_right);
    return write(root_, rootbuf);
  }
  return insert_separator(path, depth - 1, promoted, new_right);
}

// --- erase ------------------------------------------------------------------

Status BTree::erase(std::span<const std::byte> key, bool& existed) {
  existed = false;
  if (writer_ == nullptr) return Status::invalid_argument("b-tree opened read-only");
  std::vector<PathEntry> path;
  Bytes leaf;
  if (Status s = find_leaf(key, path, leaf); !s.ok()) return s;
  Node n(leaf, page_size_);
  const std::size_t slot = path.back().slot;
  if (slot >= n.count() || compare(n.key(slot), key) != 0) return Status();
  existed = true;
  if (Status s = free_cell_overflow(leaf, slot); !s.ok()) return s;
  n.remove_cell(slot);
  const PageNo leaf_page = path.back().page;
  if (n.count() > 0 || path.size() == 1) return write(leaf_page, leaf);

  // Empty non-root leaf: unlink from its siblings, free it, and remove its
  // reference from the parent.
  const PageNo prev = n.prev();
  const PageNo next = n.right();
  Bytes nb;
  if (prev != 0) {
    if (Status s = read(prev, nb); !s.ok()) return s;
    Node(nb, page_size_).set_right(next);
    if (Status s = write(prev, nb); !s.ok()) return s;
  }
  if (next != 0) {
    if (Status s = read(next, nb); !s.ok()) return s;
    Node(nb, page_size_).set_prev(prev);
    if (Status s = write(next, nb); !s.ok()) return s;
  }
  if (Status s = writer_->free_page(leaf_page); !s.ok()) return s;
  return remove_child(path, path.size() - 2);
}

Status BTree::remove_child(std::vector<PathEntry>& path, std::size_t depth) {
  const PageNo page = path[depth].page;
  const std::size_t slot = path[depth].slot;
  Bytes buf;
  if (Status s = read(page, buf); !s.ok()) return s;
  Node n(buf, page_size_);
  if (slot < n.count()) {
    n.remove_cell(slot);
  } else if (n.count() == 0) {
    // The only child is gone: this node goes too.
    if (depth == 0) {
      Bytes rootbuf(page_size_);
      Node::init(rootbuf, page_size_, kLeaf);
      return write(root_, rootbuf);
    }
    if (Status s = writer_->free_page(page); !s.ok()) return s;
    return remove_child(path, depth - 1);
  } else {
    // The rightmost child is gone: the last cell's child becomes rightmost.
    n.set_right(n.child(n.count() - 1));
    n.remove_cell(n.count() - 1);
  }
  if (n.count() > 0) return write(page, buf);

  // Only the rightmost child remains: collapse this node into it.
  const PageNo only = n.right();
  if (depth == 0) {
    Bytes child;
    if (Status s = read(only, child); !s.ok()) return s;
    if (Status s = write(root_, child); !s.ok()) return s;
    // If the child was a leaf its siblings are none (it is the only leaf);
    // if interior, its children are unaffected by the page number change.
    return writer_->free_page(only);
  }
  Bytes gb;
  const PageNo gp = path[depth - 1].page;
  const std::size_t gslot = path[depth - 1].slot;
  if (Status s = read(gp, gb); !s.ok()) return s;
  Node g(gb, page_size_);
  if (gslot < g.count()) {
    g.set_child(gslot, only);
  } else {
    g.set_right(only);
  }
  if (Status s = write(gp, gb); !s.ok()) return s;
  return writer_->free_page(page);
}

// --- cursor -----------------------------------------------------------------

Status BTree::Cursor::load(PageNo leaf, std::size_t slot) {
  leaf_ = leaf;
  slot_ = slot;
  if (Status s = tree_.read(leaf_, page_); !s.ok()) return s;
  return load_slot();
}

Status BTree::Cursor::load_slot() {
  Node n(page_, tree_.page_size_);
  while (slot_ >= n.count()) {
    const PageNo next = n.right();
    if (next == 0) {
      valid_ = false;
      return Status();
    }
    leaf_ = next;
    slot_ = 0;
    if (Status s = tree_.read(leaf_, page_); !s.ok()) return s;
    n = Node(page_, tree_.page_size_);
  }
  const auto k = n.key(slot_);
  key_.assign(k.begin(), k.end());
  valid_ = true;
  return Status();
}

Status BTree::Cursor::seek(std::span<const std::byte> key) {
  std::vector<PathEntry> path;
  Bytes leaf;
  if (Status s = tree_.find_leaf(key, path, leaf); !s.ok()) return s;
  page_ = std::move(leaf);
  leaf_ = path.back().page;
  slot_ = path.back().slot;
  return load_slot();
}

Status BTree::Cursor::seek_first() {
  PageNo page = tree_.root_;
  Bytes buf;
  for (int i = 0; i < 64; ++i) {
    if (Status s = tree_.read(page, buf); !s.ok()) return s;
    Node n(buf, tree_.page_size_);
    if (n.is_leaf()) {
      page_ = std::move(buf);
      leaf_ = page;
      slot_ = 0;
      return load_slot();
    }
    page = n.count() > 0 ? n.child(0) : n.right();
  }
  return Status::corrupt("b-tree too deep");
}

Status BTree::Cursor::seek_last() {
  PageNo page = tree_.root_;
  Bytes buf;
  for (int i = 0; i < 64; ++i) {
    if (Status s = tree_.read(page, buf); !s.ok()) return s;
    Node n(buf, tree_.page_size_);
    if (n.is_leaf()) {
      page_ = std::move(buf);
      leaf_ = page;
      if (n.count() == 0) {
        valid_ = false;
        return Status();
      }
      slot_ = n.count() - 1;
      const auto k = n.key(slot_);
      key_.assign(k.begin(), k.end());
      valid_ = true;
      return Status();
    }
    page = n.right();
  }
  return Status::corrupt("b-tree too deep");
}

Status BTree::Cursor::next() {
  if (!valid_) return Status();
  ++slot_;
  return load_slot();
}

Status BTree::Cursor::prev() {
  if (!valid_) return Status();
  Node n(page_, tree_.page_size_);
  while (slot_ == 0) {
    const PageNo prev = n.prev();
    if (prev == 0) {
      valid_ = false;
      return Status();
    }
    leaf_ = prev;
    if (Status s = tree_.read(leaf_, page_); !s.ok()) return s;
    n = Node(page_, tree_.page_size_);
    slot_ = n.count();
  }
  --slot_;
  const auto k = n.key(slot_);
  key_.assign(k.begin(), k.end());
  return Status();
}

Result<Bytes> BTree::Cursor::value() {
  if (!valid_) return Status::invalid_argument("cursor not positioned");
  Node n(page_, tree_.page_size_);
  if (n.overflowed(slot_)) {
    Bytes v;
    if (Status s = tree_.read_overflow(n.overflow_page(slot_), n.value_len(slot_), v); !s.ok()) return s;
    return v;
  }
  const auto v = n.inline_value(slot_);
  return Bytes(v.begin(), v.end());
}

Status BTree::for_each(
    const std::function<Status(std::span<const std::byte>, std::span<const std::byte>)>& fn) {
  Cursor c = cursor();
  if (Status s = c.seek_first(); !s.ok()) return s;
  while (c.valid()) {
    auto v = c.value();
    if (!v.ok()) return v.status();
    if (Status s = fn(c.key(), v.value()); !s.ok()) return s;
    if (Status s = c.next(); !s.ok()) return s;
  }
  return Status();
}

// --- check ------------------------------------------------------------------

Status BTree::check_node(PageNo page, std::uint32_t depth, const Bytes* lower, const Bytes* upper,
                         BTreeCheckReport& r, std::uint32_t& leaf_depth, PageNo& expected_prev,
                         std::uint64_t& entries) {
  auto problem = [&](const std::string& p) { r.problems.push_back("page " + std::to_string(page) + ": " + p); };
  if (std::find(r.pages.begin(), r.pages.end(), page) != r.pages.end()) {
    problem("reached twice");
    return Status();
  }
  r.pages.push_back(page);
  if (depth > 64) {
    problem("too deep");
    return Status();
  }
  Bytes buf;
  if (Status s = read(page, buf); !s.ok()) {
    problem(s.to_string());
    return Status();
  }
  Node n(buf, page_size_);
  if (n.type() != kLeaf && n.type() != kInterior) {
    problem("bad node type");
    return Status();
  }
  if (kHdr + 2 * n.count() > n.content_start() || n.content_start() > n.usable) {
    problem("slot array and content overlap");
    return Status();
  }
  Bytes prev_key;
  for (std::size_t i = 0; i < n.count(); ++i) {
    const std::size_t off = n.slot(i);
    if (off < n.content_start() || off + n.cell_size(i) > n.usable) {
      problem("cell " + std::to_string(i) + " out of bounds");
      return Status();
    }
    const auto k = n.key(i);
    if (i > 0 && compare(prev_key, k) >= 0) problem("keys not strictly increasing at " + std::to_string(i));
    if (lower != nullptr && compare(k, *lower) < 0) problem("key below lower bound at " + std::to_string(i));
    if (upper != nullptr && compare(k, *upper) >= 0) problem("key at or above upper bound at " + std::to_string(i));
    prev_key.assign(k.begin(), k.end());
  }
  if (n.is_leaf()) {
    ++r.leaf_pages;
    entries += n.count();
    if (leaf_depth == 0) {
      leaf_depth = depth;
    } else if (leaf_depth != depth) {
      problem("leaf at depth " + std::to_string(depth) + " differs from " + std::to_string(leaf_depth));
    }
    if (n.prev() != expected_prev) {
      problem("prev link " + std::to_string(n.prev()) + " expected " + std::to_string(expected_prev));
    }
    if (expected_prev != 0) {
      Bytes pb;
      if (read(expected_prev, pb).ok() && Node(pb, page_size_).right() != page) {
        problem("previous leaf's next link does not point here");
      }
    }
    expected_prev = page;
    if (n.count() == 0 && page != root_) problem("empty non-root leaf");
    for (std::size_t i = 0; i < n.count(); ++i) {
      if (!n.overflowed(i)) continue;
      // Walk the chain and account its pages.
      const std::size_t cap = page_size_ - kPageTrailerBytes - kOverflowHdr;
      std::size_t remaining = n.value_len(i);
      PageNo p = n.overflow_page(i);
      Bytes ob;
      std::size_t hops = 0;
      while (remaining > 0 && p != 0 && hops < 1u << 20) {
        if (std::find(r.pages.begin(), r.pages.end(), p) != r.pages.end()) {
          problem("overflow page " + std::to_string(p) + " reached twice");
          break;
        }
        r.pages.push_back(p);
        ++r.overflow_pages;
        if (Status s = read(p, ob); !s.ok()) {
          problem("overflow page " + std::to_string(p) + ": " + s.to_string());
          break;
        }
        remaining -= std::min(cap, remaining);
        p = get_u64(ob.data());
        ++hops;
      }
      if (remaining > 0) problem("overflow chain for cell " + std::to_string(i) + " ends early");
      if (remaining == 0 && p != 0) problem("overflow chain for cell " + std::to_string(i) + " has extra pages");
    }
    return Status();
  }
  ++r.interior_pages;
  if (n.right() == 0) problem("interior node without rightmost child");
  if (n.count() == 0 && page != root_) problem("interior node with no separators");
  for (std::size_t i = 0; i < n.count(); ++i) {
    const Bytes k(n.key(i).begin(), n.key(i).end());
    const Bytes* lo = i == 0 ? lower : nullptr;
    Bytes prev_sep;
    if (i > 0) {
      prev_sep.assign(n.key(i - 1).begin(), n.key(i - 1).end());
      lo = &prev_sep;
    }
    if (Status s = check_node(n.child(i), depth + 1, lo, &k, r, leaf_depth, expected_prev, entries); !s.ok()) return s;
  }
  Bytes last;
  const Bytes* lo = lower;
  if (n.count() > 0) {
    last.assign(n.key(n.count() - 1).begin(), n.key(n.count() - 1).end());
    lo = &last;
  }
  if (n.right() != 0) {
    if (Status s = check_node(n.right(), depth + 1, lo, upper, r, leaf_depth, expected_prev, entries); !s.ok()) return s;
  }
  return Status();
}

Result<BTreeCheckReport> BTree::check() {
  BTreeCheckReport r;
  std::uint32_t leaf_depth = 0;
  PageNo expected_prev = 0;
  std::uint64_t entries = 0;
  if (Status s = check_node(root_, 1, nullptr, nullptr, r, leaf_depth, expected_prev, entries); !s.ok()) return s;
  // The last leaf must have no next link.
  if (expected_prev != 0) {
    Bytes lb;
    if (read(expected_prev, lb).ok() && Node(lb, page_size_).right() != 0) {
      r.problems.push_back("last leaf has a dangling next link");
    }
  }
  r.entries = entries;
  r.depth = leaf_depth;
  r.ok = r.problems.empty();
  return r;
}

}  // namespace archivum::engine
