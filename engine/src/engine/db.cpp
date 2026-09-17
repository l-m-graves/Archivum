#include "archivum/engine/db.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <list>
#include <random>
#include <unordered_set>

#include "wal.h"

namespace archivum::engine {
namespace {

std::string directory_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return ".";
  if (slash == 0) return path.substr(0, 1);
  return path.substr(0, slash);
}

std::array<std::byte, 16> random_db_id() {
  std::random_device rd;
  std::array<std::byte, 16> id{};
  for (auto& b : id) b = static_cast<std::byte>(rd() & 0xFFu);
  return id;
}

}  // namespace

struct Db::Impl {
  std::unique_ptr<File> file;
  std::unique_ptr<Wal> wal;
  std::mutex state_mu;   // index, cache, counters
  std::mutex writer_mu;  // one write transaction (or checkpoint) at a time
  std::uint64_t active_readers = 0;
  std::uint64_t checkpoints = 0;
  std::uint64_t archived_segments = 0;
  WalRecoveryInfo recovery;
  std::array<std::byte, 16> db_id{};
  bool closed = false;

  // Clean-page cache keyed by (page, frame); frame 0 means the data file's
  // copy. Cleared at checkpoint because the data file's copies change.
  using Key = std::pair<PageNo, FrameNo>;
  struct Entry {
    std::vector<std::byte> data;
    std::list<Key>::iterator lru;
  };
  std::map<Key, Entry> cache;
  std::list<Key> lru;
  std::size_t capacity = 1024;

  const std::vector<std::byte>* cache_get(const Key& key) {
    auto it = cache.find(key);
    if (it == cache.end()) return nullptr;
    lru.splice(lru.begin(), lru, it->second.lru);
    return &it->second.data;
  }
  void cache_put(const Key& key, std::vector<std::byte> data) {
    auto it = cache.find(key);
    if (it != cache.end()) {
      it->second.data = std::move(data);
      lru.splice(lru.begin(), lru, it->second.lru);
      return;
    }
    lru.push_front(key);
    cache[key] = Entry{std::move(data), lru.begin()};
    while (cache.size() > capacity && !lru.empty()) {
      cache.erase(lru.back());
      lru.pop_back();
    }
  }
  void cache_clear() {
    cache.clear();
    lru.clear();
  }
};

// ---------------------------------------------------------------------------

Db::Db(Vfs& vfs, std::string path, DbOptions options)
    : vfs_(vfs),
      path_(std::move(path)),
      wal_path_(path_ + ".wal"),
      options_(options),
      impl_(std::make_unique<Impl>()) {
  impl_->capacity = std::max<std::size_t>(8, options.cache_pages);
}

Db::~Db() { (void)close(); }

Result<std::unique_ptr<Db>> Db::open(Vfs& vfs, std::string path, DbOptions options) {
  if (options.page_size < kMinPageSize || options.page_size > kMaxPageSize ||
      (options.page_size & (options.page_size - 1)) != 0) {
    return Status::invalid_argument("page_size must be a power of two in [512, 65536]");
  }
  std::unique_ptr<Db> db(new Db(vfs, std::move(path), options));
  auto exists = vfs.exists(db->path_);
  if (!exists.ok()) return exists.status();
  if (!exists.value() && !options.create_if_missing) return Status::not_found(db->path_);

  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  auto file = vfs.open(db->path_, flags);
  if (!file.ok()) return file.status();
  db->impl_->file = std::move(file).value();
  File& f = *db->impl_->file;

  auto init = [&]() -> Status {
    DbHeader h;
    h.page_size = options.page_size;
    h.page_count = 1;
    h.db_id = random_db_id();
    std::vector<std::byte> page(options.page_size, std::byte{0});
    h.encode(page);
    seal_page(page);
    if (Status s = f.truncate(0); !s.ok()) return s;
    if (Status s = f.write(0, page); !s.ok()) return s;
    if (Status s = f.sync(); !s.ok()) return s;
    if (Status s = vfs.sync_directory(directory_of(db->path_)); !s.ok()) return s;
    // A stale log cannot belong to a fresh file.
    auto wal_exists = vfs.exists(db->wal_path_);
    if (wal_exists.ok() && wal_exists.value()) {
      if (Status s = vfs.remove(db->wal_path_); !s.ok()) return s;
      if (Status s = vfs.sync_directory(directory_of(db->path_)); !s.ok()) return s;
    }
    return Status();
  };

  auto size_r = f.size();
  if (!size_r.ok()) return size_r.status();
  bool fresh = !exists.value() || size_r.value() == 0;
  if (fresh) {
    if (Status s = init(); !s.ok()) return s;
  }

  // Read page 0. The page size is in the header, so read the first
  // kMinPageSize bytes to learn it, then the whole page.
  std::vector<std::byte> probe(kMinPageSize, std::byte{0});
  std::size_t got = 0;
  if (Status s = f.read(0, probe, got); !s.ok()) return s;
  // A short read leaves zeros, which decode() rejects; the torn-header rule
  // below then decides between re-initialising and reporting corruption.
  auto hdr = got >= DbHeader::kEncodedBytes ? DbHeader::decode(probe)
                                            : Result<DbHeader>(Status::corrupt("database file too short"));
  std::uint32_t page_size = hdr.ok() ? hdr.value().page_size : options.page_size;
  std::vector<std::byte> page0(page_size, std::byte{0});
  if (Status s = f.read(0, page0, got); !s.ok()) return s;
  size_r = f.size();
  if (!size_r.ok()) return size_r.status();
  if (!hdr.ok() || got != page_size || !page_checksum_ok(page0)) {
    // The file's page 0 is unusable. Two legitimate causes:
    //   1. A crash during checkpoint tore the copy of page 0 in the data
    //      file. The log still holds the committed page 0 (the log is
    //      emptied only after the data file is synced), so recover the
    //      log, identified by its own header, and take page 0 from it.
    //   2. A crash during creation, before any transaction could have been
    //      acknowledged: the file is at most one page and there is no log
    //      with a page 0. Re-initialise.
    // Anything else is corruption and is refused.
    auto from_wal = db->recover_page0_from_wal(page0);
    if (!from_wal.ok()) return from_wal.status();
    if (from_wal.value()) {
      hdr = DbHeader::decode(page0);
      if (!hdr.ok()) return hdr.status();
    } else if (size_r.value() <= page_size) {
      db->impl_->wal.reset();
      if (Status s = init(); !s.ok()) return s;
      if (Status s = f.read(0, page0, got); !s.ok()) return s;
      hdr = DbHeader::decode(page0);
      if (!hdr.ok()) return hdr.status();
    } else {
      return Status::corrupt("database header invalid and not recoverable from the log: " + db->path_);
    }
  }
  db->page_size_ = hdr.value().page_size;
  db->impl_->db_id = hdr.value().db_id;

  // Durability barrier (invariant I7). The data file is written by
  // creation and by checkpoints only, and a commit syncs just the log. If
  // an earlier open failed after writing page 0 but before syncing it, a
  // retry would otherwise build acknowledged commits on a header that is
  // not on disk, and a crash would then lose the file and, with it, the
  // log that belongs to it. Two syncs per open is the price.
  if (Status s = f.sync(); !s.ok()) return s;
  if (Status s = vfs.sync_directory(directory_of(db->path_)); !s.ok()) return s;

  if (!db->impl_->wal) {
    WalOpenOptions wo;
    wo.db_id = db->impl_->db_id;
    wo.new_base_change_counter = hdr.value().change_counter;
    auto wal = Wal::open(vfs, db->wal_path_, db->page_size_, wo, &db->impl_->recovery);
    if (!wal.ok()) return wal.status();
    db->impl_->wal = std::move(wal).value();
  }
  // The log must continue from this file. Either its base is the file's
  // change counter, or a crash landed between a checkpoint's file sync and
  // its log reset, in which case the file is ahead of the base but no
  // further than the log's last commit. Anything else means the file and
  // the log belong to different histories (a restored backup beside a live
  // log, for instance) and replaying would corrupt silently.
  {
    const Wal& wal = *db->impl_->wal;
    const std::uint64_t file_cc = hdr.value().change_counter;
    const std::uint64_t base = wal.base_change_counter();
    const bool continues = base == file_cc ||
                           (!wal.commits().empty() && base < file_cc && file_cc <= wal.commits().back().change_counter);
    if (!continues) {
      return Status::corrupt("log " + db->wal_path_ + " (base change counter " + std::to_string(base) +
                             ") does not continue database " + db->path_ + " (change counter " +
                             std::to_string(file_cc) + "); restore the matching log or remove it deliberately");
    }
  }
  return db;
}

// Opens the log using the identity in its own header and, if it holds a
// committed copy of page 0, returns it in `page0` and keeps the log open.
// Returns false when the log cannot supply page 0 (absent, unreadable
// header, no committed frame for it); propagates I/O errors, including a
// simulated crash, so the caller does not mistake them for corruption.
Result<bool> Db::recover_page0_from_wal(std::span<std::byte> page0) {
  auto exists = vfs_.exists(wal_path_);
  if (!exists.ok()) return exists.status();
  if (!exists.value()) return false;
  auto wf = vfs_.open(wal_path_, OpenFlags{});
  if (!wf.ok()) return wf.status();
  std::array<std::byte, WalHeader::kEncodedBytes> hbuf{};
  std::size_t got = 0;
  if (Status s = wf.value()->read(0, hbuf, got); !s.ok()) return s;
  if (Status s = wf.value()->close(); !s.ok()) return s;
  if (got != hbuf.size()) return false;
  auto wh = WalHeader::decode(hbuf);
  if (!wh.ok()) return false;
  if (wh.value().page_size != page0.size()) return false;
  WalOpenOptions wo;
  wo.db_id = wh.value().db_id;
  wo.new_base_change_counter = wh.value().base_change_counter;
  auto wal = Wal::open(vfs_, wal_path_, wh.value().page_size, wo, &impl_->recovery);
  if (!wal.ok()) return wal.status();
  const FrameNo frame = wal.value()->find(0, wal.value()->last_commit());
  if (frame == 0) return false;
  if (Status s = wal.value()->read_frame_page(frame, page0); !s.ok()) return s;
  impl_->wal = std::move(wal).value();
  return true;
}

const std::array<std::byte, 16>& Db::db_id() const { return impl_->db_id; }

std::int64_t Db::now_us() const {
  if (options_.now_us != nullptr) return options_.now_us();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Db::archive_segment_name(const std::array<std::byte, 16>& db_id, std::uint64_t base_change_counter) {
  static const char* hex = "0123456789abcdef";
  std::string name;
  for (std::byte b : db_id) {
    name += hex[std::to_integer<unsigned>(b) >> 4];
    name += hex[std::to_integer<unsigned>(b) & 15];
  }
  std::string n = std::to_string(base_change_counter);
  name += "-" + std::string(20 - std::min<std::size_t>(20, n.size()), '0') + n + ".wal";
  return name;
}

Result<std::uint64_t> Db::backup(Vfs& dst_vfs, const std::string& dst_path) {
  auto txn_r = begin_read();
  if (!txn_r.ok()) return txn_r.status();
  auto& txn = *txn_r.value();
  OpenFlags flags;
  flags.write = true;
  flags.create = true;
  flags.truncate = true;
  auto out = dst_vfs.open(dst_path, flags);
  if (!out.ok()) return out.status();
  std::vector<std::byte> page(page_size_);
  for (PageNo p = 0; p < txn.page_count(); ++p) {
    if (Status s = txn.read_page(p, page); !s.ok()) return s;
    if (Status s = out.value()->write(p * page_size_, page); !s.ok()) return s;
  }
  if (Status s = out.value()->sync(); !s.ok()) return s;
  if (Status s = out.value()->close(); !s.ok()) return s;
  if (Status s = dst_vfs.sync_directory(directory_of(dst_path)); !s.ok()) return s;
  return txn.change_counter();
}

Status Db::close() {
  if (!impl_ || impl_->closed) return Status();
  impl_->closed = true;
  Status first;
  if (impl_->wal) {
    if (Status s = impl_->wal->close(); !s.ok() && first.ok()) first = s;
  }
  if (impl_->file) {
    if (Status s = impl_->file->close(); !s.ok() && first.ok()) first = s;
  }
  return first;
}

Status Db::read_committed(PageNo page, FrameNo snapshot, std::span<std::byte> out) {
  if (out.size() != page_size_) return Status::invalid_argument("page buffer size");
  std::lock_guard<std::mutex> lock(impl_->state_mu);
  if (impl_->closed) return Status::io("database closed");
  const FrameNo frame = impl_->wal->find(page, snapshot);
  const Impl::Key key{page, frame};
  if (const auto* cached = impl_->cache_get(key)) {
    std::copy(cached->begin(), cached->end(), out.begin());
    return Status();
  }
  if (frame != 0) {
    if (Status s = impl_->wal->read_frame_page(frame, out); !s.ok()) return s;
  } else {
    std::size_t got = 0;
    if (Status s = impl_->file->read(page * page_size_, out, got); !s.ok()) return s;
    if (got != page_size_) {
      return Status::corrupt("page " + std::to_string(page) + " beyond end of file: " + path_);
    }
    if (!page_checksum_ok(out)) {
      return Status::corrupt("page " + std::to_string(page) + " checksum mismatch: " + path_);
    }
  }
  impl_->cache_put(key, std::vector<std::byte>(out.begin(), out.end()));
  return Status();
}

Result<DbHeader> Db::committed_header(FrameNo snapshot) {
  std::vector<std::byte> page0(page_size_);
  if (Status s = read_committed(0, snapshot, page0); !s.ok()) return s;
  return DbHeader::decode(page0);
}

Result<std::unique_ptr<ReadTxn>> Db::begin_read() {
  FrameNo snapshot = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mu);
    if (impl_->closed) return Status::io("database closed");
    snapshot = impl_->wal->last_commit();
  }
  auto header = committed_header(snapshot);
  if (!header.ok()) return header.status();
  {
    std::lock_guard<std::mutex> lock(impl_->state_mu);
    ++impl_->active_readers;
  }
  return std::unique_ptr<ReadTxn>(
      new ReadTxn(*this, snapshot, header.value().page_count, header.value().change_counter));
}

void Db::reader_ended() {
  std::lock_guard<std::mutex> lock(impl_->state_mu);
  --impl_->active_readers;
}

Result<std::unique_ptr<WriteTxn>> Db::begin_write() {
  std::unique_lock<std::mutex> writer(impl_->writer_mu);
  FrameNo snapshot = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mu);
    if (impl_->closed) return Status::io("database closed");
    if (impl_->wal->failed()) {
      return Status::io("database refuses writes after a log I/O error; reopen it: " + path_);
    }
    snapshot = impl_->wal->last_commit();
  }
  auto header = committed_header(snapshot);
  if (!header.ok()) return header.status();
  return std::unique_ptr<WriteTxn>(new WriteTxn(*this, snapshot, header.value(), std::move(writer)));
}

void Db::writer_ended() {}

Status Db::commit_frames(std::vector<std::pair<PageNo, std::vector<std::byte>>>& images,
                         const DbHeader& header, std::uint64_t db_size) {
  (void)header;
  std::vector<Wal::PageImage> frames;
  frames.reserve(images.size());
  for (auto& [page, data] : images) frames.push_back(Wal::PageImage{page, data});
  bool want_checkpoint = false;
  {
    std::lock_guard<std::mutex> lock(impl_->state_mu);
    if (impl_->closed) return Status::io("database closed");
    const FrameNo first = impl_->wal->frame_count() + 1;
    if (Status s = impl_->wal->append_transaction(frames, db_size); !s.ok()) return s;
    for (std::size_t i = 0; i < images.size(); ++i) {
      impl_->cache_put(Impl::Key{images[i].first, first + i}, std::move(images[i].second));
    }
    want_checkpoint = impl_->wal->frame_count() >= options_.checkpoint_threshold_frames &&
                      impl_->active_readers == 0;
  }
  if (want_checkpoint) {
    // Best effort: a Busy or I/O failure here does not fail the commit,
    // which is already durable.
    (void)checkpoint_locked();
  }
  return Status();
}

Status Db::checkpoint() {
  std::unique_lock<std::mutex> writer(impl_->writer_mu);
  return checkpoint_locked();
}

Status Db::checkpoint_locked() {
  std::lock_guard<std::mutex> lock(impl_->state_mu);
  if (impl_->closed) return Status::io("database closed");
  if (impl_->active_readers != 0) return Status::busy("checkpoint deferred: active readers");
  Wal& wal = *impl_->wal;
  const FrameNo last = wal.last_commit();
  if (last == 0) return Status();
  const auto frames = wal.latest_frames(last);
  std::vector<std::byte> buf(page_size_);
  for (const auto& [page, frame] : frames) {
    if (Status s = wal.read_frame_page(frame, buf); !s.ok()) return s;
    if (Status s = impl_->file->write(page * page_size_, buf); !s.ok()) return s;
  }
  const std::uint64_t db_size = wal.db_size_at(last);
  auto size_r = impl_->file->size();
  if (!size_r.ok()) return size_r.status();
  if (size_r.value() > db_size * page_size_) {
    if (Status s = impl_->file->truncate(db_size * page_size_); !s.ok()) return s;
  }
  // The data file must be durable before the log that made it so goes away.
  if (Status s = impl_->file->sync(); !s.ok()) return s;
  const std::uint64_t committed_cc = wal.commits().back().change_counter;
  // Archive the log before it is emptied; the segment is named by the
  // base it continues from so recovery can chain segments.
  if (!options_.archive_dir.empty()) {
    const std::string seg = options_.archive_dir + "/" + archive_segment_name(impl_->db_id, wal.base_change_counter());
    if (Status s = wal.copy_committed_to(vfs_, seg); !s.ok()) return s;
    ++impl_->archived_segments;
  }
  // Whatever reset() manages to do, cached copies keyed by frame are stale
  // from here on; drop them first.
  impl_->cache_clear();
  if (Status s = wal.reset(committed_cc); !s.ok()) return s;
  ++impl_->checkpoints;
  return Status();
}

Result<CheckReport> Db::check() {
  CheckReport report;
  auto txn_r = begin_read();
  if (!txn_r.ok()) return txn_r.status();
  auto& txn = *txn_r.value();
  std::vector<std::byte> page(page_size_);
  for (PageNo p = 0; p < txn.page_count(); ++p) {
    Status s = txn.read_page(p, page);
    if (!s.ok()) {
      report.problems.push_back("page " + std::to_string(p) + ": " + s.to_string());
    }
    ++report.pages_checked;
  }
  if (Status s = txn.read_page(0, page); s.ok()) {
    auto hdr = DbHeader::decode(page);
    if (!hdr.ok()) {
      report.problems.push_back("header: " + hdr.status().to_string());
    } else {
      std::unordered_set<PageNo> seen;
      PageNo cur = hdr.value().freelist_head;
      while (cur != 0) {
        if (cur >= hdr.value().page_count) {
          report.problems.push_back("free list references page " + std::to_string(cur) +
                                    " beyond page count");
          break;
        }
        if (!seen.insert(cur).second) {
          report.problems.push_back("free list cycle at page " + std::to_string(cur));
          break;
        }
        if (Status s2 = txn.read_page(cur, page); !s2.ok()) {
          report.problems.push_back("free page " + std::to_string(cur) + ": " + s2.to_string());
          break;
        }
        auto fp = FreePage::decode(page);
        if (!fp.ok()) {
          report.problems.push_back("free page " + std::to_string(cur) + ": " + fp.status().to_string());
          break;
        }
        ++report.free_pages_walked;
        report.free_pages.push_back(cur);
        cur = fp.value().next;
      }
      if (report.free_pages_walked != hdr.value().freelist_count) {
        report.problems.push_back("free list count " + std::to_string(report.free_pages_walked) +
                                  " differs from header " + std::to_string(hdr.value().freelist_count));
      }
    }
  }
  report.ok = report.problems.empty();
  return report;
}

DbStats Db::stats() const {
  std::lock_guard<std::mutex> lock(impl_->state_mu);
  DbStats s;
  s.wal_frames = impl_->wal ? impl_->wal->frame_count() : 0;
  s.checkpoints = impl_->checkpoints;
  s.archived_segments = impl_->archived_segments;
  s.active_readers = impl_->active_readers;
  s.wal_recovered_frames = impl_->recovery.committed_frames;
  s.wal_dropped_tail_bytes = impl_->recovery.dropped_bytes;
  return s;
}

// ---------------------------------------------------------------------------

ReadTxn::ReadTxn(Db& db, FrameNo snapshot, std::uint64_t page_count, std::uint64_t change_counter)
    : db_(db), snapshot_(snapshot), page_count_(page_count), change_counter_(change_counter) {}

ReadTxn::~ReadTxn() { db_.reader_ended(); }

Status ReadTxn::read_page(PageNo page, std::span<std::byte> out) const {
  if (page >= page_count_) return Status::invalid_argument("page " + std::to_string(page) + " out of range");
  return db_.read_committed(page, snapshot_, out);
}

// ---------------------------------------------------------------------------

WriteTxn::WriteTxn(Db& db, FrameNo snapshot, DbHeader header, std::unique_lock<std::mutex> lock)
    : db_(db), snapshot_(snapshot), header_(header), snapshot_header_(header), lock_(std::move(lock)) {}

WriteTxn::~WriteTxn() {
  if (!finished_) rollback();
}

void WriteTxn::finish() {
  finished_ = true;
  dirty_.clear();
  freed_.clear();
  if (lock_.owns_lock()) lock_.unlock();
  db_.writer_ended();
}

Status WriteTxn::read_page(PageNo page, std::span<std::byte> out) const {
  if (finished_) return Status::invalid_argument("transaction finished");
  if (out.size() != db_.page_size()) return Status::invalid_argument("page buffer size");
  if (page >= header_.page_count) return Status::invalid_argument("page out of range");
  auto it = dirty_.find(page);
  if (it != dirty_.end()) {
    std::copy(it->second.begin(), it->second.end(), out.begin());
    return Status();
  }
  if (page >= snapshot_header_.page_count) {
    return Status::invalid_argument("page allocated but never written in this transaction");
  }
  return db_.read_committed(page, snapshot_, out);
}

Status WriteTxn::write_page(PageNo page, std::span<const std::byte> data) {
  if (finished_) return Status::invalid_argument("transaction finished");
  if (page == 0) return Status::invalid_argument("page 0 is the header and not writable");
  if (page >= header_.page_count) return Status::invalid_argument("page out of range");
  if (data.size() != db_.page_size()) return Status::invalid_argument("page data size");
  if (freed_.count(page) != 0) return Status::invalid_argument("page was freed in this transaction");
  dirty_[page].assign(data.begin(), data.end());
  return Status();
}

Result<PageNo> WriteTxn::allocate_page() {
  if (finished_) return Status::invalid_argument("transaction finished");
  PageNo page = 0;
  if (header_.freelist_head != 0) {
    page = header_.freelist_head;
    std::vector<std::byte> buf(db_.page_size());
    if (Status s = read_page(page, buf); !s.ok()) return s;
    auto fp = FreePage::decode(buf);
    if (!fp.ok()) return Status::corrupt("free list head page " + std::to_string(page) + " invalid");
    header_.freelist_head = fp.value().next;
    --header_.freelist_count;
    freed_.erase(page);
  } else {
    page = header_.page_count;
    ++header_.page_count;
  }
  dirty_[page].assign(db_.page_size(), std::byte{0});
  return page;
}

Status WriteTxn::free_page(PageNo page) {
  if (finished_) return Status::invalid_argument("transaction finished");
  if (page == 0 || page >= header_.page_count) return Status::invalid_argument("page out of range");
  if (freed_.count(page) != 0) return Status::invalid_argument("page freed twice");
  std::vector<std::byte> buf(db_.page_size(), std::byte{0});
  FreePage fp;
  fp.next = header_.freelist_head;
  fp.encode(buf);
  dirty_[page] = std::move(buf);
  freed_.insert(page);
  header_.freelist_head = page;
  ++header_.freelist_count;
  return Status();
}

Status WriteTxn::commit() {
  if (finished_) return Status::invalid_argument("transaction finished");
  const bool header_changed = header_.page_count != snapshot_header_.page_count ||
                              header_.freelist_head != snapshot_header_.freelist_head ||
                              header_.freelist_count != snapshot_header_.freelist_count;
  if (dirty_.empty() && !header_changed) {
    finish();
    return Status();
  }
  header_.change_counter = snapshot_header_.change_counter + 1;
  header_.commit_time_us = db_.now_us();
  std::vector<std::pair<PageNo, std::vector<std::byte>>> images;
  images.reserve(dirty_.size() + 1);
  for (auto& [page, data] : dirty_) {
    seal_page(data);
    images.emplace_back(page, std::move(data));
  }
  std::vector<std::byte> page0(db_.page_size(), std::byte{0});
  header_.encode(page0);
  seal_page(page0);
  images.emplace_back(0, std::move(page0));
  Status s = db_.commit_frames(images, header_, header_.page_count);
  finish();
  return s;
}

void WriteTxn::rollback() {
  if (finished_) return;
  finish();
}

}  // namespace archivum::engine
