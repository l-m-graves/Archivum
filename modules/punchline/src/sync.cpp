#include "archivum/punchline/sync.h"

#include <algorithm>
#include <map>
#include <set>

#include "archivum/core/ids.h"
#include "archivum/punchline/exceptions.h"
#include "archivum/punchline/localtime.h"
#include "archivum/punchline/rules.h"

namespace archivum::punchline {
namespace {
using engine::Bound;
using engine::Row;
using engine::Value;

bool later_than_submitted(const std::string& state) { return state == "approved" || state == "released" || state == "locked"; }

Result<bool> pay_code_active(engine::Reader& r, const std::string& code) {
  auto row = r.get("pay_codes", {Value::text(code)});
  if (!row.ok()) return row.status();
  return row.value().has_value() && (*row.value())[3].as_bool();
}

// Every current (not superseded) entry of the employee at or after `from_us`,
// in device-time order.
Result<std::vector<TimeEntry>> current_entries_from(engine::Reader& r, std::int64_t employee_id, std::int64_t from_us) {
  std::vector<TimeEntry> out;
  Status s = r.scan("time_entries", "time_entries_employee_time", Bound{{Value::integer(employee_id), Value::timestamp(from_us)}},
                    Bound{{Value::integer(employee_id)}}, false, [&](const Row& row) {
                      TimeEntry e = TimeEntry::from_row(row);
                      if (e.superseded_by == 0) out.push_back(std::move(e));
                      return true;
                    });
  if (!s.ok()) return s;
  std::stable_sort(out.begin(), out.end(), [](const TimeEntry& a, const TimeEntry& b) {
    return a.device_time != b.device_time ? a.device_time < b.device_time : a.id < b.id;
  });
  return out;
}

// Shifts of the employee that end at or after `from_us`, or are open.
Result<std::vector<Shift>> shifts_from(engine::Reader& r, std::int64_t employee_id, std::int64_t from_us) {
  std::vector<Shift> out;
  for (const char* state : {"recorded", "submitted", "approved", "released", "locked"}) {
    Status s = r.scan("shifts", "shifts_employee_state", Bound{{Value::integer(employee_id), Value::text(state)}},
                      Bound{{Value::integer(employee_id), Value::text(state)}}, false, [&](const Row& row) {
                        Shift sh = Shift::from_row(row);
                        if (sh.out_entry_id == 0 || sh.out_time >= from_us || sh.in_time >= from_us) out.push_back(std::move(sh));
                        return true;
                      });
    if (!s.ok()) return s;
  }
  std::sort(out.begin(), out.end(), [](const Shift& a, const Shift& b) { return a.in_time < b.in_time; });
  return out;
}

void note_opened(std::vector<std::int64_t>* opened, const Opened& o) {
  if (opened != nullptr && o.created) opened->push_back(o.id);
}

// Schedule checks for a closed shift, on the in punch's local day.
Status check_schedule(core::Recorder& rec, const Config& cfg, const TimeEntry& in, const TimeEntry& out, std::int64_t now_us,
                      std::vector<std::int64_t>* opened) {
  auto lt_in = parse_local_time(in.local_time);
  auto lt_out = parse_local_time(out.local_time);
  if (!lt_in.ok() || !lt_out.ok()) return Status();  // validated at insert; a stored oddity is not this check's problem
  auto scheds = schedules_of(rec.writer(), in.employee_id);
  if (!scheds.ok()) return scheds.status();
  std::vector<Schedule> in_force;
  for (const Schedule& s : scheds.value()) {
    if (s.effective_from_day <= lt_in.value().local_day && (s.effective_to_day == 0 || lt_in.value().local_day < s.effective_to_day)) {
      in_force.push_back(s);
    }
  }
  ExceptionKey key;
  key.employee_id = in.employee_id;
  key.entry_id = in.id;
  if (in_force.empty()) {
    key.kind = "no_schedule";
    auto o = open_exception(rec, key, "no schedule on file for " + format_local_day(lt_in.value().local_day), now_us);
    if (!o.ok()) return o.status();
    note_opened(opened, o.value());
    return Status();
  }
  const Schedule* day = nullptr;
  for (const Schedule& s : in_force) {
    if (s.weekday == lt_in.value().weekday) day = &s;
  }
  if (day == nullptr) {
    key.kind = "non_scheduled_day";
    auto o = open_exception(rec, key, "punch on " + format_local_day(lt_in.value().local_day) + ", a non-scheduled day", now_us);
    if (!o.ok()) return o.status();
    note_opened(opened, o.value());
    return Status();
  }
  const std::int64_t tol = cfg.schedule_tolerance_minutes;
  const bool in_early = lt_in.value().minute_of_day < day->start_minute - tol;
  // The out punch is compared only when it is on the same local day.
  const bool out_late = lt_out.value().local_day == lt_in.value().local_day && lt_out.value().minute_of_day > day->end_minute + tol;
  if (in_early || out_late) {
    key.kind = "outside_schedule";
    auto o = open_exception(rec, key,
                            std::string(in_early ? "in punch before" : "out punch after") + " the scheduled hours on " +
                                format_local_day(lt_in.value().local_day),
                            now_us);
    if (!o.ok()) return o.status();
    note_opened(opened, o.value());
  }
  return Status();
}

Status count_device_attested(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t period_id, std::int64_t now_us,
                             std::vector<std::int64_t>* opened) {
  if (period_id == 0 || cfg.device_attested_threshold <= 0) return Status();
  auto entries = entries_in_period(rec.writer(), period_id, employee_id);
  if (!entries.ok()) return entries.status();
  std::int64_t n = 0;
  for (const TimeEntry& e : entries.value()) {
    if (e.attestation == "device" && e.superseded_by == 0) ++n;
  }
  if (n < cfg.device_attested_threshold) return Status();
  ExceptionKey key;
  key.kind = "device_attested_count";
  key.employee_id = employee_id;
  key.period_id = period_id;
  auto o = open_exception(rec, key, std::to_string(n) + " device-attested entries in the period (threshold " +
                                        std::to_string(cfg.device_attested_threshold) + ")", now_us);
  if (!o.ok()) return o.status();
  note_opened(opened, o.value());
  return Status();
}

Status validate_incoming(const IncomingEntry& e) {
  if (e.kind != "in" && e.kind != "out") return Status::invalid_argument("kind must be in or out");
  if (e.device_time_us <= 0) return Status::invalid_argument("device_time_us must be positive");
  if (e.journal_sequence < 0) return Status::invalid_argument("journal_sequence must be non-negative");
  if (auto lt = parse_local_time(e.local_time); !lt.ok()) return lt.status();
  if (e.site_zone.empty() || e.site_zone.size() > 64) return Status::invalid_argument("site_zone is required");
  if (e.tzdb_version.empty() || e.tzdb_version.size() > 32) return Status::invalid_argument("tzdb_version is required");
  if (e.note.size() > 4096) return Status::invalid_argument("note is too long");
  if (e.pay_code.size() > 64) return Status::invalid_argument("pay_code is too long");
  return Status();
}

}  // namespace

Status repair_pairing(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t from_us, std::int64_t now_us,
                      std::vector<std::int64_t>* opened) {
  engine::Writer& w = rec.writer();
  // Shifts approved or later are settled: never rebuilt. A punch arriving
  // for a settled span is folded on its own (and ends up flagged as
  // unpaired, for the exception queue), so a late sync never fails and
  // never alters what a supervisor approved.
  //
  // The rebuild range is closed under overlap: it starts at `from_us`,
  // extends back to the in punch of every rebuildable shift it touches
  // (open, or ending at or after the start), and repeats until nothing
  // more is touched, so that a shift is either wholly rebuilt or wholly
  // kept and no entry of a kept shift is folded twice.
  std::vector<Shift> affected;
  std::int64_t start = from_us;
  while (true) {
    auto touched = shifts_from(w, employee_id, start);
    if (!touched.ok()) return touched.status();
    std::int64_t new_start = start;
    affected.clear();
    for (const Shift& sh : touched.value()) {
      if (later_than_submitted(sh.state)) continue;
      affected.push_back(sh);
      new_start = std::min(new_start, sh.in_time);
    }
    if (new_start == start) break;
    start = new_start;
  }
  for (const Shift& sh : affected) {
    // The pairing exceptions of the rebuilt shifts are resolved and reopened
    // by the fold below if still warranted.
    for (const char* kind : {"unpaired_punch", "long_shift", "no_schedule", "non_scheduled_day", "outside_schedule"}) {
      for (std::int64_t entry : {sh.in_entry_id, sh.out_entry_id}) {
        if (entry == 0) continue;
        ExceptionKey key;
        key.kind = kind;
        key.employee_id = employee_id;
        key.entry_id = entry;
        if (auto r = resolve_matching(rec, key, "pairing", now_us); !r.ok()) return r.status();
      }
    }
    if (Status s = rec.remove("shifts", {Value::integer(sh.id)}); !s.ok()) return s;
  }
  auto all_entries = current_entries_from(w, employee_id, start);
  if (!all_entries.ok()) return all_entries.status();
  Result<std::vector<TimeEntry>> entries = std::vector<TimeEntry>();
  for (const TimeEntry& e : all_entries.value()) {
    if (!later_than_submitted(e.state)) entries.value().push_back(e);  // settled entries keep their settled shifts
  }
  // Unpaired-punch exceptions on entries in the rebuilt range are resolved
  // first; the fold reopens the ones still unpaired.
  for (const TimeEntry& e : entries.value()) {
    ExceptionKey key;
    key.kind = "unpaired_punch";
    key.employee_id = employee_id;
    key.entry_id = e.id;
    if (auto r = resolve_matching(rec, key, "pairing", now_us); !r.ok()) return r.status();
  }
  std::optional<Shift> open;
  std::optional<TimeEntry> open_in;
  auto flag_unpaired = [&](const TimeEntry& e, const char* why) -> Status {
    ExceptionKey key;
    key.kind = "unpaired_punch";
    key.employee_id = employee_id;
    key.entry_id = e.id;
    key.device_id = e.device_id;
    auto o = open_exception(rec, key, why, now_us);
    if (!o.ok()) return o.status();
    note_opened(opened, o.value());
    return Status();
  };
  // An open shift interrupted by a later punch is a missed out punch. The
  // shift open at the end of the fold is someone clocked in, which is not
  // an exception until it has been open for the long-shift threshold (the
  // monitor re-checks open shifts on every tick).
  auto persist_open = [&](bool interrupted) -> Status {
    if (!open) return Status();
    auto id = core::next_id(w, "shifts");
    if (!id.ok()) return id.status();
    open->id = id.value();
    if (Status s = rec.insert("shifts", open->to_row()); !s.ok()) return s;
    const bool stale = now_us - open->in_time >= cfg.long_shift_hours * 3600 * 1'000'000;
    if (interrupted || stale) {
      if (Status s = flag_unpaired(*open_in, interrupted ? "in punch without an out punch" : "in punch still open past the long-shift threshold");
          !s.ok()) {
        return s;
      }
    }
    open.reset();
    open_in.reset();
    return Status();
  };
  for (const TimeEntry& e : entries.value()) {
    if (e.kind == "in") {
      if (open) {
        if (Status s = persist_open(true); !s.ok()) return s;  // the earlier in stays open and flagged
      }
      auto lt = parse_local_time(e.local_time);
      Shift sh;
      sh.employee_id = employee_id;
      sh.device_id = e.device_id;
      sh.in_entry_id = e.id;
      sh.local_day = lt.ok() ? lt.value().local_day : 0;
      sh.period_id = e.period_id;
      sh.in_time = e.device_time;
      sh.pay_code = e.pay_code;
      sh.state = e.state;
      open = sh;
      open_in = e;
      continue;
    }
    // An out punch.
    if (!open || !rules::pair_out(*open, e).ok()) {
      if (open && open->device_id != e.device_id) {
        // Out from another device: the open shift stays open, this out is unpaired.
        if (Status s = flag_unpaired(e, "out punch from a different device than the open in punch"); !s.ok()) return s;
        continue;
      }
      if (open) {
        if (Status s = persist_open(true); !s.ok()) return s;
      }
      if (Status s = flag_unpaired(e, "out punch without an in punch"); !s.ok()) return s;
      continue;
    }
    open->out_entry_id = e.id;
    open->out_time = e.device_time;
    open->duration_us = e.device_time - open->in_time;
    if (open->pay_code.empty()) open->pay_code = e.pay_code;
    // A late-arriving out for a submitted in keeps the in's state (the
    // approve step refuses mixed states, so nothing reaches payroll
    // without a resubmission).
    auto id = core::next_id(w, "shifts");
    if (!id.ok()) return id.status();
    open->id = id.value();
    if (Status s = rec.insert("shifts", open->to_row()); !s.ok()) return s;
    if (*open->duration_us >= cfg.long_shift_hours * 3600 * 1'000'000) {
      ExceptionKey key;
      key.kind = "long_shift";
      key.employee_id = employee_id;
      key.entry_id = open->in_entry_id;
      auto o = open_exception(rec, key, "shift of " + std::to_string(*open->duration_us / 3'600'000'000) + " hours", now_us);
      if (!o.ok()) return o.status();
      note_opened(opened, o.value());
    }
    if (Status s = check_schedule(rec, cfg, *open_in, e, now_us, opened); !s.ok()) return s;
    open.reset();
    open_in.reset();
  }
  return persist_open(false);
}

Result<SyncOutcome> apply_batch(core::Recorder& rec, const Config& cfg, const Device& device, const IncomingBatch& batch,
                                std::int64_t now_us) {
  engine::Writer& w = rec.writer();
  SyncOutcome out;
  if (static_cast<std::int64_t>(batch.entries.size()) > cfg.max_batch_entries) {
    return Status::invalid_argument("batch exceeds " + std::to_string(cfg.max_batch_entries) + " entries");
  }
  auto seen = batch_by_uuid(w, batch.batch_uuid);
  if (!seen.ok()) return seen.status();
  out.replayed = seen.value().has_value();
  if (batch.client_time_us != 0) {
    out.clock_divergence_us = now_us - batch.client_time_us;
    const std::int64_t tol = cfg.clock_divergence_tolerance_seconds * 1'000'000;
    if (*out.clock_divergence_us > tol || *out.clock_divergence_us < -tol) {
      ExceptionKey key;
      key.kind = "clock_divergence";
      key.device_id = device.id;
      key.employee_id = device.employee_id;
      auto o = open_exception(rec, key, "device clock differs from the server's by " + std::to_string(*out.clock_divergence_us / 1'000'000) + " s",
                              now_us);
      if (!o.ok()) return o.status();
      note_opened(&out.exceptions_opened, o.value());
    }
  }
  // Entries in journal order; duplicates inside one batch are one row.
  std::vector<IncomingEntry> entries = batch.entries;
  std::stable_sort(entries.begin(), entries.end(),
                   [](const IncomingEntry& a, const IncomingEntry& b) { return a.journal_sequence < b.journal_sequence; });
  std::set<engine::UuidBytes> in_batch;
  std::int64_t earliest_new = 0;
  std::set<std::int64_t> periods_touched;
  std::int64_t first_seq = -1, last_seq = -1;
  for (const IncomingEntry& in : entries) {
    EntryOutcome oc;
    oc.entry_uuid = in.entry_uuid;
    oc.journal_sequence = in.journal_sequence;
    auto reject = [&](const std::string& why) {
      oc.accepted = false;
      oc.reason = why;
      ++out.rejected;
      out.outcomes.push_back(oc);
    };
    auto accept = [&]() {
      oc.accepted = true;
      ++out.accepted;
      out.outcomes.push_back(oc);
      out.acked_sequences.push_back(in.journal_sequence);
      if (first_seq < 0 || in.journal_sequence < first_seq) first_seq = in.journal_sequence;
      if (in.journal_sequence > last_seq) last_seq = in.journal_sequence;
    };
    if (in_batch.count(in.entry_uuid)) {
      accept();  // the same uuid twice in one payload is a client-side duplicate
      continue;
    }
    auto existing = entry_by_uuid(w, in.entry_uuid);
    if (!existing.ok()) return existing.status();
    if (existing.value().has_value()) {
      in_batch.insert(in.entry_uuid);
      if (existing.value()->device_id != device.id) {
        reject("entry uuid belongs to another device");
        continue;
      }
      accept();  // replay after an outage
      continue;
    }
    if (Status s = validate_incoming(in); !s.ok()) {
      reject(s.message());
      continue;
    }
    // (journal id, sequence) already used by a different entry: the client
    // journal reissued a number (a lying disk); the uuid is the truth.
    {
      auto dup = w.scan_all("time_entries", "time_entries_journal_sequence",
                            Bound{{Value::uuid(batch.journal_id), Value::integer(in.journal_sequence)}},
                            Bound{{Value::uuid(batch.journal_id), Value::integer(in.journal_sequence)}});
      if (!dup.ok()) return dup.status();
      if (!dup.value().empty()) {
        reject("journal sequence " + std::to_string(in.journal_sequence) + " is already stored under another entry uuid");
        continue;
      }
    }
    if (!in.pay_code.empty()) {
      auto active = pay_code_active(w, in.pay_code);
      if (!active.ok()) return active.status();
      if (!active.value()) {
        reject("unknown or inactive pay code '" + in.pay_code + "'");
        continue;
      }
    }
    if (Status s = rules::device_attested_valid(w, device.id, now_us); !s.ok()) {
      if (s.code() != ErrorCode::Constraint) return s;
      reject(s.message());
      continue;
    }
    const LocalTime lt = parse_local_time(in.local_time).value();
    auto period = period_for_day(w, lt.local_day);
    if (!period.ok()) return period.status();
    TimeEntry e;
    auto id = core::next_id(w, "time_entries");
    if (!id.ok()) return id.status();
    e.id = id.value();
    e.entry_uuid = in.entry_uuid;
    e.employee_id = device.employee_id;  // the enrollment record's, never the request's
    e.device_id = device.id;
    e.journal_id = batch.journal_id;
    e.journal_sequence = in.journal_sequence;
    e.kind = in.kind;
    e.device_time = in.device_time_us;
    e.local_time = in.local_time;
    e.site_zone = in.site_zone;
    e.tzdb_version = in.tzdb_version;
    e.receipt_time = now_us;
    e.attestation = "device";
    e.clock_divergence_us = out.clock_divergence_us;
    e.period_id = period.value() && period.value()->state == "open" ? period.value()->id : 0;
    e.state = "recorded";
    e.pay_code = in.pay_code;
    e.created_at = now_us;
    e.note = in.note;
    if (Status s = rules::entry_period_valid(w, lt.local_day, e.period_id); !s.ok()) return s;
    if (Status s = rec.insert("time_entries", e.to_row()); !s.ok()) return s;
    in_batch.insert(in.entry_uuid);
    accept();
    if (earliest_new == 0 || e.device_time < earliest_new) earliest_new = e.device_time;
    if (e.period_id != 0) periods_touched.insert(e.period_id);
    if (period.value() && period.value()->state == "locked") {
      ExceptionKey key;
      key.kind = "past_cutoff";
      key.employee_id = e.employee_id;
      key.entry_id = e.id;
      key.period_id = period.value()->id;
      auto o = open_exception(rec, key, "punch received for a locked pay period; needs a retroactive adjustment", now_us);
      if (!o.ok()) return o.status();
      note_opened(&out.exceptions_opened, o.value());
    }
  }
  if (earliest_new != 0) {
    if (Status s = repair_pairing(rec, cfg, device.employee_id, earliest_new, now_us, &out.exceptions_opened); !s.ok()) return s;
  }
  for (std::int64_t period : periods_touched) {
    if (Status s = count_device_attested(rec, cfg, device.employee_id, period, now_us, &out.exceptions_opened); !s.ok()) return s;
  }
  for (const IncomingReport& r : batch.reports) {
    if (r.kind != "retry_exhausted" && r.kind != "journal_recovery") {
      return Status::invalid_argument("report kind must be retry_exhausted or journal_recovery");
    }
    ExceptionKey key;
    key.kind = r.kind;
    key.device_id = device.id;
    key.employee_id = device.employee_id;
    auto o = open_exception(rec, key, r.detail.substr(0, 512), now_us);
    if (!o.ok()) return o.status();
    note_opened(&out.exceptions_opened, o.value());
  }
  // The device has been heard from: freshness restored, acknowledgement state advanced.
  {
    Device d = device;
    d.last_seen_at = now_us;
    d.last_journal_id = batch.journal_id;
    for (std::int64_t seq : out.acked_sequences) d.last_acked_sequence = std::max(d.last_acked_sequence, seq);
    if (Status s = rec.update("devices", d.to_row()); !s.ok()) return s;
    ExceptionKey stale;
    stale.kind = "device_stale";
    stale.device_id = device.id;
    stale.employee_id = device.employee_id;
    if (auto r = resolve_matching(rec, stale, "sync", now_us); !r.ok()) return r.status();
    out.last_acked_sequence = d.last_acked_sequence;
  }
  SyncBatch b;
  if (out.replayed) {
    b = *seen.value();
    b.accepted = out.accepted;
    b.rejected = out.rejected;
    b.received_at = now_us;
    if (Status s = rec.update("sync_batches", b.to_row()); !s.ok()) return s;
  } else {
    auto id = core::next_id(w, "sync_batches");
    if (!id.ok()) return id.status();
    b.id = id.value();
    b.batch_uuid = batch.batch_uuid;
    b.device_id = device.id;
    b.received_at = now_us;
    b.first_sequence = std::max<std::int64_t>(first_seq, 0);
    b.last_sequence = std::max<std::int64_t>(last_seq, 0);
    b.accepted = out.accepted;
    b.rejected = out.rejected;
    b.client_time_us = batch.client_time_us;
    b.clock_divergence_us = out.clock_divergence_us;
    b.journal_id = batch.journal_id;
    if (Status s = rec.insert("sync_batches", b.to_row()); !s.ok()) return s;
  }
  std::sort(out.acked_sequences.begin(), out.acked_sequences.end());
  out.acked_sequences.erase(std::unique(out.acked_sequences.begin(), out.acked_sequences.end()), out.acked_sequences.end());
  return out;
}

Result<TimeEntry> record_manual_entry(core::Recorder& rec, const Config& cfg, const ManualEntry& m, std::int64_t now_us) {
  engine::Writer& w = rec.writer();
  if (m.kind != "in" && m.kind != "out") return Status::invalid_argument("kind must be in or out");
  if (m.device_time_us <= 0) return Status::invalid_argument("device_time_us must be positive");
  auto lt = parse_local_time(m.local_time);
  if (!lt.ok()) return lt.status();
  if (m.site_zone.empty() || m.tzdb_version.empty()) return Status::invalid_argument("site_zone and tzdb_version are required");
  if (m.reason.empty()) return Status::constraint("PL-4: a manual entry needs a reason");
  auto emp = employee_by_id(w, m.employee_id);
  if (!emp.ok()) return emp.status();
  if (!emp.value().has_value()) return Status::not_found("no such employee");
  std::int64_t device_id = 0;
  std::optional<TimeEntry> original;
  if (m.correction_of != 0) {
    if (Status s = rules::correction_valid(w, m.correction_of, m.employee_id, m.reason); !s.ok()) return s;
    auto o = entry_by_id(w, m.correction_of);
    if (!o.ok()) return o.status();
    original = o.value();
    if (original->superseded_by != 0) return Status::constraint("PL-4: that entry has already been corrected");
    if (later_than_submitted(original->state)) {
      return Status::constraint("PL-4: an entry that is " + original->state + " takes a retroactive adjustment, not a correction");
    }
    device_id = original->device_id;
  } else {
    // A missing punch: attributed to the employee's active device, or the
    // most recent one.
    Device best;
    Status s = w.scan("devices", "devices_employee", Bound{{Value::integer(m.employee_id)}}, Bound{{Value::integer(m.employee_id)}}, false,
                      [&](const Row& row) {
                        Device d = Device::from_row(row);
                        if (best.id == 0 || d.revoked_at == 0 || d.enrolled_at > best.enrolled_at) best = d;
                        return true;
                      });
    if (!s.ok()) return s;
    if (best.id == 0) return Status::constraint("employee has no device to attribute a manual entry to");
    device_id = best.id;
  }
  if (!m.pay_code.empty()) {
    auto active = pay_code_active(w, m.pay_code);
    if (!active.ok()) return active.status();
    if (!active.value()) return Status::invalid_argument("unknown or inactive pay code '" + m.pay_code + "'");
  }
  auto period = period_for_day(w, lt.value().local_day);
  if (!period.ok()) return period.status();
  if (period.value() && period.value()->state == "locked") {
    return Status::constraint("pay period is locked; a retroactive adjustment is the payroll console's action");
  }
  auto id = core::next_id(w, "time_entries");
  if (!id.ok()) return id.status();
  TimeEntry e;
  e.id = id.value();
  e.entry_uuid = engine::UuidBytes{};
  // A manual entry's uuid is derived from its id (unique, and never a client's).
  for (std::size_t i = 0; i < 8; ++i) e.entry_uuid[15 - i] = static_cast<std::byte>((static_cast<std::uint64_t>(e.id) >> (8 * i)) & 0xFF);
  e.entry_uuid[0] = std::byte{0xEE};
  e.employee_id = m.employee_id;
  e.device_id = device_id;
  e.journal_sequence = 0;
  e.kind = m.kind;
  e.device_time = m.device_time_us;
  e.local_time = m.local_time;
  e.site_zone = m.site_zone;
  e.tzdb_version = m.tzdb_version;
  e.receipt_time = now_us;
  e.attestation = "manual";
  e.period_id = period.value() ? period.value()->id : 0;
  e.state = "recorded";
  e.pay_code = m.pay_code;
  e.correction_of = m.correction_of;
  e.correction_reason = m.reason;
  e.created_at = now_us;
  e.note = m.note;
  if (Status s = rules::entry_period_valid(w, lt.value().local_day, e.period_id); !s.ok()) return s;
  if (Status s = rec.insert("time_entries", e.to_row()); !s.ok()) return s;
  std::int64_t from = e.device_time;
  if (original) {
    original->superseded_by = e.id;
    if (Status s = rec.update("time_entries", original->to_row()); !s.ok()) return s;
    from = std::min(from, original->device_time);
    // The corrected entry's own exceptions are moot.
    for (const char* kind : {"unpaired_punch", "long_shift", "no_schedule", "non_scheduled_day", "outside_schedule"}) {
      ExceptionKey key;
      key.kind = kind;
      key.employee_id = m.employee_id;
      key.entry_id = original->id;
      if (auto r = resolve_matching(rec, key, "correction", now_us); !r.ok()) return r.status();
      key.device_id = original->device_id;
      if (auto r = resolve_matching(rec, key, "correction", now_us); !r.ok()) return r.status();
    }
  }
  if (Status s = repair_pairing(rec, cfg, m.employee_id, from, now_us); !s.ok()) return s;
  return e;
}

}  // namespace archivum::punchline
