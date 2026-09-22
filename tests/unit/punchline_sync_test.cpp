// Sync, pairing, corrections and the approval lifecycle at the module
// level: engine only, one Recorder per "request", no server. The wire
// layer (tests/server/server_punchline_test.cpp) is thin over this.
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <set>

#include "archivum/core/module.h"
#include "archivum/core/schema.h"
#include "archivum/punchline/exceptions.h"
#include "archivum/punchline/lifecycle.h"
#include "archivum/punchline/localtime.h"
#include "archivum/punchline/rollback.h"
#include "archivum/punchline/schema.h"
#include "archivum/punchline/sync.h"
#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;
using namespace archivum::punchline;

namespace {

constexpr std::int64_t kUs = 1'000'000;
// 2024-03-04T00:00:00 UTC; local day 19786 (a Monday). The site zone is UTC
// so local wall clocks and instants agree in this test.
constexpr std::int64_t kMonday = 1'709'510'400LL * kUs;

UuidBytes uid(int n) {
  UuidBytes u{};
  u[15] = static_cast<std::byte>(n & 0xFF);
  u[14] = static_cast<std::byte>((n >> 8) & 0xFF);
  return u;
}

struct World {
  MemVfs vfs;
  std::unique_ptr<Store> store;
  core::RecordPolicy policy;
  Config cfg;
  Device device;
  std::int64_t now = kMonday + 8 * 3600 * kUs;

  void open() {
    auto st = Store::open(vfs, "w/punch.db");
    REQUIRE_OK(st.status());
    store = std::move(st.value());
    REQUIRE_OK(core::migrate_all(*store, {&module()}).status());
    policy = core::build_policy({&module()});
    cfg.long_shift_hours = 12;
    cfg.device_attested_threshold = 6;
    auto w = store->begin_write();
    REQUIRE_OK(w.status());
    core::Actor sys;
    sys.account = "seed";
    core::Recorder rec(*w.value(), policy, sys, "seed", 1);
    REQUIRE_OK(rec.status());
    for (int i = 1; i <= 2; ++i) {
      Employee e;
      e.id = i;
      e.employee_number = "E" + std::to_string(i);
      e.display_name = i == 1 ? "Worker" : "Boss";
      e.site_zone = "UTC";
      e.created_at = e.updated_at = 1;
      REQUIRE_OK(rec.insert("employees", e.to_row()));
    }
    device.id = 1;
    device.device_uuid = uid(900);
    device.name = "kiosk";
    device.employee_id = 1;
    device.credential_hash = {std::byte{1}};
    device.enrolled_at = 1;
    device.enrolled_by = "seed";
    REQUIRE_OK(rec.insert("devices", device.to_row()));
    SupervisorAssignment a;
    a.id = 1;
    a.employee_id = 1;
    a.supervisor_employee_id = 2;
    a.effective_from = 1;
    a.assigned_by = "seed";
    a.audit_id = rec.audit_id();
    REQUIRE_OK(rec.insert("supervisor_assignments", a.to_row()));
    PayPeriod p;
    p.id = 1;
    p.start_day = 19786;
    p.end_day = 19792;
    p.site_zone = "UTC";
    p.tzdb_version = "2024a";
    p.submit_by = kMonday + 8 * 86400 * kUs;
    p.approve_by = kMonday + 9 * 86400 * kUs;
    REQUIRE_OK(rec.insert("pay_periods", p.to_row()));
    for (int wd = 1; wd <= 5; ++wd) {
      Schedule s;
      s.id = wd;
      s.employee_id = 1;
      s.weekday = wd;
      s.start_minute = 9 * 60;
      s.end_minute = 17 * 60;
      s.effective_from_day = 19000;
      REQUIRE_OK(rec.insert("schedules", s.to_row()));
    }
    REQUIRE_OK(w.value()->commit());
  }

  IncomingEntry punch(int n, int seq, const char* kind, int day_offset, int hour, int minute = 0) {
    IncomingEntry e;
    e.entry_uuid = uid(n);
    e.journal_sequence = seq;
    e.kind = kind;
    e.device_time_us = kMonday + (day_offset * 86400 + hour * 3600 + minute * 60) * kUs;
    e.local_time = format_local_day(19786 + day_offset) + "T" + (hour < 10 ? "0" : "") + std::to_string(hour) + ":" +
                   (minute < 10 ? "0" : "") + std::to_string(minute) + ":00";
    e.site_zone = "UTC";
    e.tzdb_version = "2024a";
    return e;
  }

  Result<SyncOutcome> sync(const IncomingBatch& b) {
    auto w = store->begin_write();
    if (!w.ok()) return w.status();
    core::Actor dev;
    dev.kind = core::Actor::Kind::Device;
    dev.device = device.device_uuid;
    core::Recorder rec(*w.value(), policy, dev, "sync.batch", now);
    if (!rec.status().ok()) return rec.status();
    auto d = device_by_id(*w.value(), device.id);
    if (!d.ok()) return d.status();
    auto r = apply_batch(rec, cfg, *d.value(), b, now);
    if (!r.ok()) return r.status();
    if (Status s = w.value()->commit(); !s.ok()) return s;
    return r;
  }

  template <class F>
  Status act(const char* action, F fn) {
    auto w = store->begin_write();
    if (!w.ok()) return w.status();
    core::Actor sys;
    sys.account = "test";
    core::Recorder rec(*w.value(), policy, sys, action, now);
    if (!rec.status().ok()) return rec.status();
    if (Status s = fn(rec); !s.ok()) return s;
    return w.value()->commit();
  }

  std::vector<Exception> open_kinds(const std::string& kind = "") {
    auto rd = store->begin_read();
    auto x = open_exceptions(*rd.value(), {}, kind);
    return x.ok() ? x.value() : std::vector<Exception>();
  }
  std::vector<Shift> shifts() {
    auto rd = store->begin_read();
    auto rows = rd.value()->scan_all("shifts");
    std::vector<Shift> out;
    for (const Row& r : rows.value()) out.push_back(Shift::from_row(r));
    return out;
  }
  std::vector<TimeEntry> entries() {
    auto rd = store->begin_read();
    auto rows = rd.value()->scan_all("time_entries");
    std::vector<TimeEntry> out;
    for (const Row& r : rows.value()) out.push_back(TimeEntry::from_row(r));
    return out;
  }
  std::uint64_t audit_rows(const std::string& action) {
    auto rd = store->begin_read();
    std::uint64_t n = 0;
    (void)rd.value()->scan("audit_log", "", std::nullopt, std::nullopt, false, [&](const Row& r) {
      if (r[core::audit::kAction].as_text() == action) ++n;
      return true;
    });
    return n;
  }
};

}  // namespace

ARCHIVUM_TEST(sync_accepts_pairs_and_is_idempotent) {
  World w;
  w.open();
  IncomingBatch b;
  b.batch_uuid = uid(100);
  b.journal_id = uid(500);
  b.client_time_us = w.now - 3 * kUs;
  b.entries = {w.punch(1, 1, "in", 0, 8, 55), w.punch(2, 2, "out", 0, 17, 5)};
  auto r = w.sync(b);
  REQUIRE_OK(r.status());
  CHECK(r.value().accepted == 2 && r.value().rejected == 0 && !r.value().replayed);
  CHECK(r.value().acked_sequences == std::vector<std::int64_t>({1, 2}));
  CHECK(r.value().last_acked_sequence == 2);
  CHECK(r.value().clock_divergence_us.value() == 3 * kUs);
  CHECK(r.value().exceptions_opened.empty());
  auto shifts = w.shifts();
  REQUIRE(shifts.size() == 1);
  CHECK(shifts[0].out_entry_id != 0 && shifts[0].duration_us.value() == (8 * 3600 + 10 * 60) * kUs);
  CHECK(shifts[0].local_day == 19786 && shifts[0].period_id == 1 && shifts[0].state == "recorded");
  auto entries = w.entries();
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].attestation == "device" && entries[0].period_id == 1 && entries[0].employee_id == 1);
  CHECK(entries[0].journal_id.value() == uid(500) && entries[0].receipt_time == w.now);
  CHECK(w.open_kinds().empty());
  // Replay after an outage: accepted again, nothing duplicated, batch marked replayed.
  auto again = w.sync(b);
  REQUIRE_OK(again.status());
  CHECK(again.value().replayed && again.value().accepted == 2);
  CHECK(w.entries().size() == 2 && w.shifts().size() == 1);
  CHECK(w.audit_rows("sync.batch") == 2);
  // The device's acknowledgement state advanced and freshness is recorded.
  auto rd = w.store->begin_read();
  auto d = device_by_id(*rd.value(), 1);
  CHECK(d.value()->last_acked_sequence == 2 && d.value()->last_seen_at == w.now && d.value()->last_journal_id.value() == uid(500));
}

ARCHIVUM_TEST(sync_rejects_per_entry_and_flags_the_exception_queue) {
  World w;
  w.open();
  IncomingBatch b;
  b.batch_uuid = uid(101);
  b.journal_id = uid(500);
  b.client_time_us = w.now - 11 * 60 * kUs;  // 11 minutes off: beyond the 5 minute tolerance
  IncomingEntry bad = w.punch(9, 9, "lunch", 1, 12);
  IncomingEntry dup = w.punch(3, 3, "in", 1, 9);
  IncomingEntry reused_seq = w.punch(77, 3, "in", 1, 9, 30);  // sequence 3 again, another uuid
  b.entries = {w.punch(3, 3, "in", 1, 9),    dup,
               w.punch(4, 4, "in", 1, 13),   // a second in: the first stays open and unpaired
               w.punch(5, 5, "out", 1, 17),  // closes the 13:00 in
               bad,
               reused_seq,
               w.punch(6, 6, "out", 2, 10),      // out with no in
               w.punch(7, 7, "in", 5, 10),       // Saturday
               w.punch(8, 8, "out", 5, 12),
               w.punch(10, 10, "in", 3, 6),      // 06:00 to 23:30: long and outside schedule
               w.punch(11, 11, "out", 3, 23, 30)};
  b.reports = {{"retry_exhausted", "gave up after 5 attempts"}};
  auto r = w.sync(b);
  REQUIRE_OK(r.status());
  CHECK(r.value().accepted == 9 && r.value().rejected == 2);
  std::set<std::string> reasons;
  for (const EntryOutcome& o : r.value().outcomes) {
    if (!o.accepted) reasons.insert(o.reason);
  }
  CHECK(reasons.size() == 2);
  bool kind_reason = false, seq_reason = false;
  for (const std::string& s : reasons) {
    if (s.find("kind") != std::string::npos) kind_reason = true;
    if (s.find("sequence 3") != std::string::npos) seq_reason = true;
  }
  CHECK(kind_reason && seq_reason);
  CHECK(w.entries().size() == 8);
  auto shifts = w.shifts();
  int open_shifts = 0, closed = 0;
  for (const Shift& s : shifts) (s.out_entry_id == 0 ? open_shifts : closed)++;
  CHECK_MSG(open_shifts == 1 && closed == 3, open_shifts << " open, " << closed << " closed");
  std::multiset<std::string> kinds;
  for (const Exception& x : w.open_kinds()) kinds.insert(x.kind);
  CHECK(kinds.count("unpaired_punch") == 2);  // the 09:00 in and the Wednesday out
  CHECK(kinds.count("non_scheduled_day") == 1);
  CHECK(kinds.count("long_shift") == 1);
  CHECK(kinds.count("outside_schedule") == 1);
  CHECK(kinds.count("clock_divergence") == 1);
  CHECK(kinds.count("retry_exhausted") == 1);
  CHECK(kinds.count("device_attested_count") == 1);  // 8 device-attested entries, threshold 6
  // Refused entries are surfaced: one item per device with the current list.
  CHECK(kinds.count("entry_rejected") == 1);
  {
    auto items = w.open_kinds("entry_rejected");
    REQUIRE(items.size() == 1);
    CHECK(items[0].device_id == 1 && items[0].detail.find("sequence 3") != std::string::npos);
    CHECK(items[0].detail.find("kind must be") != std::string::npos);
  }
  // Another sync with the same divergence does not duplicate the open item;
  // a resend of the same refused entry does not duplicate its item either.
  IncomingBatch b2;
  b2.batch_uuid = uid(102);
  b2.journal_id = uid(500);
  b2.client_time_us = b.client_time_us;
  b2.entries = {bad, reused_seq};
  auto r2 = w.sync(b2);
  REQUIRE_OK(r2.status());
  CHECK(r2.value().rejected == 2 && r2.value().accepted == 0);
  CHECK(w.open_kinds("clock_divergence").size() == 1);
  CHECK(w.open_kinds("entry_rejected").size() == 1);
}

ARCHIVUM_TEST(correction_supersedes_and_repairs_pairing) {
  World w;
  w.open();
  IncomingBatch b;
  b.batch_uuid = uid(103);
  b.journal_id = uid(500);
  b.entries = {w.punch(1, 1, "in", 0, 9), w.punch(2, 2, "in", 0, 9, 7), w.punch(3, 3, "out", 0, 17)};
  REQUIRE_OK(w.sync(b).status());
  CHECK(w.open_kinds("unpaired_punch").size() == 1);
  // The 09:00 in was a stray double punch. The supervisor corrects it with
  // a manual entry carrying the reason; the stray is superseded and
  // excluded from pairing, which is rebuilt from the earlier of the two.
  ManualEntry m;
  m.employee_id = 1;
  m.kind = "in";
  m.device_time_us = kMonday + (9 * 3600 + 7 * 60) * kUs;
  m.local_time = "2024-03-04T09:07:00";
  m.site_zone = "UTC";
  m.tzdb_version = "2024a";
  m.correction_of = 1;
  m.reason = "double punch at the door";
  TimeEntry created;
  REQUIRE_OK(w.act("entry.correct", [&](core::Recorder& rec) {
    auto r = record_manual_entry(rec, w.cfg, m, w.now);
    if (!r.ok()) return r.status();
    created = r.value();
    return Status();
  }));
  CHECK(created.attestation == "manual" && created.correction_of == 1 && !created.journal_id.has_value());
  auto entries = w.entries();
  REQUIRE(entries.size() == 4);
  CHECK(entries[0].superseded_by == created.id);
  auto shifts = w.shifts();
  // Two ins remain current (the manual 09:07 and the device 09:07): the
  // first stays open and flagged, the second pairs with 17:00.
  int open_shifts = 0;
  for (const Shift& s : shifts) open_shifts += s.out_entry_id == 0;
  CHECK_MSG(shifts.size() == 2 && open_shifts == 1, shifts.size() << " shifts");
  // A correction of an already corrected entry, or with no reason, is refused.
  m.reason = "";
  CHECK(w.act("entry.correct", [&](core::Recorder& rec) { return record_manual_entry(rec, w.cfg, m, w.now).status(); }).code() ==
        ErrorCode::Constraint);
  m.reason = "again";
  CHECK(w.act("entry.correct", [&](core::Recorder& rec) { return record_manual_entry(rec, w.cfg, m, w.now).status(); }).code() ==
        ErrorCode::Constraint);
  CHECK(w.audit_rows("entry.correct") == 1);  // refused corrections leave no audit row
}

ARCHIVUM_TEST(lifecycle_transitions_are_ordered_audited_and_reach_payroll_only_by_release) {
  World w;
  w.open();
  IncomingBatch b;
  b.batch_uuid = uid(104);
  b.journal_id = uid(500);
  b.entries = {w.punch(1, 1, "in", 0, 9), w.punch(2, 2, "out", 0, 17), w.punch(3, 3, "in", 1, 9), w.punch(4, 4, "out", 1, 17)};
  REQUIRE_OK(w.sync(b).status());
  rules::Standing self;
  self.employee_id = 1;
  rules::Standing boss;
  boss.roles = {"supervisor"};
  boss.employee_id = 2;
  rules::Standing payroll;
  payroll.roles = {"payroll"};
  auto move = [&](const char* to, const rules::Standing& who, std::string note = "") -> Result<TransitionResult> {
    TransitionResult out;
    Status s = w.act(std::string("period.") + to == "period.submitted" ? "period.submit" : "period.transition", [&](core::Recorder& rec) {
      TransitionRequest req;
      req.period_id = 1;
      req.employee_id = 1;
      req.to_state = to;
      req.who = who;
      req.acted_by_account = "test";
      req.note = std::move(note);
      req.now_us = w.now;
      auto r = transition(rec, req);
      if (!r.ok()) return r.status();
      out = r.value();
      return Status();
    });
    if (!s.ok()) return s;
    return out;
  };
  // Out of order and wrong standing are refused and leave no audit row.
  CHECK(move("approved", boss).status().code() == ErrorCode::Constraint);
  CHECK(move("submitted", boss).ok());  // the supervisor may submit for their report
  CHECK(move("submitted", self).status().code() == ErrorCode::Constraint);  // nothing left to submit
  CHECK(move("approved", self).status().code() == ErrorCode::Constraint);
  CHECK(move("approved", payroll).status().code() == ErrorCode::Constraint);  // needs an override reason
  auto approved = move("approved", boss);
  REQUIRE_OK(approved.status());
  CHECK(approved.value().entries == 4 && approved.value().shifts == 2 && approved.value().from_state == "submitted");
  for (const TimeEntry& e : w.entries()) CHECK(e.state == "approved" && e.approval_audit_id != 0);
  // A late punch after approval is accepted, stays recorded, and is flagged; approved shifts are untouched.
  IncomingBatch late;
  late.batch_uuid = uid(105);
  late.journal_id = uid(500);
  late.entries = {w.punch(5, 5, "out", 0, 18)};
  auto lr = w.sync(late);
  REQUIRE_OK(lr.status());
  CHECK(lr.value().accepted == 1);
  CHECK(w.open_kinds("unpaired_punch").size() == 1);
  CHECK(w.open_kinds("late_punch").size() == 1);  // visible to the supervisor, not just unpaired
  CHECK(w.open_kinds("late_punch")[0].period_id == 1 && w.open_kinds("late_punch")[0].employee_id == 1);
  CHECK(w.shifts().size() == 2);
  // Release needs everything approved: the late entry blocks until it is
  // submitted and approved (payroll, with an override, may do both).
  CHECK(move("released", payroll).status().code() == ErrorCode::Constraint);
  REQUIRE_OK(move("submitted", payroll).status());
  rules::Standing override = payroll;
  override.override_reason = "late punch after the supervisor's approval";
  REQUIRE_OK(move("approved", override).status());
  CHECK(w.open_kinds("late_punch").empty());  // the re-approval reviewed it
  auto released = move("released", payroll);
  REQUIRE_OK(released.status());
  CHECK(released.value().entries == 5);
  // Lock: payroll, every entry released; then nothing moves.
  LockResult lock;
  REQUIRE_OK(w.act("period.lock", [&](core::Recorder& rec) {
    auto r = lock_period(rec, 1, payroll, "", "", "test", w.now);
    if (!r.ok()) return r.status();
    lock = r.value();
    return Status();
  }));
  CHECK(lock.employees == 1 && lock.entries == 5);
  for (const TimeEntry& e : w.entries()) CHECK(e.state == "locked");
  CHECK(move("submitted", payroll).status().code() == ErrorCode::Constraint);
  // Every transition wrote an approvals row under its audit row; refused ones wrote nothing.
  auto rd = w.store->begin_read();
  auto approvals = rd.value()->scan_all("approvals");
  REQUIRE_OK(approvals.status());
  CHECK_MSG(approvals.value().size() == 6, approvals.value().size());  // submit, approve, submit, approve(override), release, lock
  std::set<std::int64_t> audit_ids;
  for (const Row& r : approvals.value()) {
    audit_ids.insert(r[10].as_int64());
    auto audit = rd.value()->get("audit_log", {r[10]});
    CHECK(audit.ok() && audit.value().has_value());
  }
  CHECK(audit_ids.size() == 6);
  bool override_noted = false;
  for (const Row& r : approvals.value()) {
    if (!r[9].is_null() && r[9].as_text().find("override") != std::string::npos) override_noted = true;
  }
  CHECK(override_noted);
  // A punch for the locked period is accepted (never lost) and flagged past_cutoff, unassigned to the period.
  IncomingBatch after_lock;
  after_lock.batch_uuid = uid(106);
  after_lock.journal_id = uid(500);
  after_lock.entries = {w.punch(6, 6, "in", 2, 9)};
  REQUIRE_OK(w.sync(after_lock).status());
  CHECK(w.open_kinds("past_cutoff").size() == 1);
  CHECK(w.entries().back().period_id == 0);
}

// The rollback export: every closed shift of the period as one old-store
// entry in the old client's batch shape, keyed by the in punch's uuid,
// with name, company and cost centre from the employee record, minutes
// computed, open shifts skipped, batched per device.
ARCHIVUM_TEST(rollback_export_rewrites_shifts_as_old_store_batches) {
  World w;
  w.open();
  {
    auto wr = w.store->begin_write();
    REQUIRE_OK(wr.status());
    auto e = employee_by_id(*wr.value(), 1);
    Employee emp = *e.value();
    emp.company = "ACME";
    emp.cost_center = "4400";
    REQUIRE_OK(wr.value()->update("employees", emp.to_row()));
    REQUIRE_OK(wr.value()->commit());
  }
  IncomingBatch b;
  b.batch_uuid = uid(110);
  b.journal_id = uid(500);
  b.entries = {w.punch(1, 1, "in", 0, 9), w.punch(2, 2, "out", 0, 17, 30), w.punch(3, 3, "in", 1, 8), w.punch(4, 4, "out", 1, 12),
               w.punch(5, 5, "in", 2, 9)};  // the last is open
  b.entries[0].note = "line 3";
  REQUIRE_OK(w.sync(b).status());
  auto rd = w.store->begin_read();
  auto ex = rollback_export(*rd.value(), 1, false, kMonday + 7 * 86400 * kUs);
  REQUIRE_OK(ex.status());
  CHECK(ex.value().shifts == 2 && ex.value().open_shifts_skipped == 1 && ex.value().employees_without_routing == 0);
  REQUIRE(ex.value().batches.size() == 1);
  const nlohmann::json& batch = ex.value().batches[0];
  CHECK(batch["device_id"] == engine::Value::uuid(uid(900)).to_string());
  CHECK(batch["client_version"] == "archivum-rollback");
  CHECK(batch["submitted_at"] == "2024-03-11T00:00:00Z");
  REQUIRE(batch["entries"].size() == 2);
  const nlohmann::json& first = batch["entries"][0];
  CHECK(first["uuid"] == engine::Value::uuid(uid(1)).to_string());
  CHECK(first["employee_id"] == "E1" && first["employee_name"] == "Worker");
  CHECK(first["company"] == "ACME" && first["cost_center"] == "4400");
  CHECK(first["clock_in"] == "2024-03-04T09:00:00Z" && first["clock_out"] == "2024-03-04T17:30:00Z");
  CHECK(first["minutes"] == 510 && first["note"] == "line 3");
  CHECK(first["archivum_state"] == "recorded");
  // Only the keys the old server names, plus the operator's state key.
  for (const auto& [k, v] : first.items()) {
    CHECK_MSG(k == "uuid" || k == "employee_id" || k == "employee_name" || k == "company" || k == "cost_center" || k == "clock_in" ||
                  k == "clock_out" || k == "minutes" || k == "note" || k == "archivum_state",
              k);
  }
  // released_only: nothing yet; after release, both.
  auto none = rollback_export(*rd.value(), 1, true, kMonday);
  REQUIRE_OK(none.status());
  CHECK(none.value().shifts == 0 && none.value().batches.empty());
  CHECK(rollback_export(*rd.value(), 42, false, kMonday).status().code() == ErrorCode::NotFound);
  // Missing routing is exported with empty strings and counted, never refused.
  rd.value().reset();
  {
    auto wr = w.store->begin_write();
    auto e = employee_by_id(*wr.value(), 1);
    Employee emp = *e.value();
    emp.cost_center = "";
    REQUIRE_OK(wr.value()->update("employees", emp.to_row()));
    REQUIRE_OK(wr.value()->commit());
  }
  rd = w.store->begin_read();
  auto missing = rollback_export(*rd.value(), 1, false, kMonday);
  REQUIRE_OK(missing.status());
  CHECK(missing.value().employees_without_routing == 2 && missing.value().batches[0]["entries"][0]["cost_center"] == "");
}
