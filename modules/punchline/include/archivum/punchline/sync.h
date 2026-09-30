// Sync and pairing: the business logic behind the sync endpoint and
// manual entries, engine-only so it is tested without the server.
//
// A batch from a device is applied in one write transaction under one
// audit row (the caller's Recorder): every accepted entry is inserted,
// pairing is redone from the earliest new punch, exceptions are opened,
// the device's acknowledgement state advances, and the batch is recorded.
// Idempotent: an entry whose uuid is already stored is accepted again
// without a second row; a replayed batch is answered the same way.
//
// Pairing (PL-6) is a fold over the employee's current entries (not
// superseded) in device-time order: an in punch opens a shift, the next
// out punch of the same device closes it. An in punch while a shift is
// open, or an out punch with none open, is `unpaired_punch`. The fold is
// rerun from a point in time whenever entries before the latest shift
// arrive or a correction lands, rebuilding only shifts still in
// `recorded` or `submitted`; shifts approved or later are never rebuilt.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "archivum/core/recorder.h"
#include "archivum/punchline/config.h"
#include "archivum/punchline/data.h"

namespace archivum::punchline {

struct IncomingEntry {
  engine::UuidBytes entry_uuid{};
  std::int64_t journal_sequence = 0;
  std::string kind;  // in | out
  std::int64_t device_time_us = 0;
  std::string local_time;  // YYYY-MM-DDTHH:MM:SS
  // Wire tolerance only: site_zone is compared with the employee record's
  // (a disagreement opens local_clock_mismatch) and tzdb_version is
  // discarded. Neither is ever stored or used.
  std::string site_zone, tzdb_version;
  std::string pay_code;  // optional
  std::string note;      // optional
  std::string break_code;  // optional: an out punch marked M begins a meal
};

struct IncomingReport {
  std::string kind;  // retry_exhausted | journal_recovery
  std::string detail;
};

struct IncomingBatch {
  engine::UuidBytes batch_uuid{};
  engine::UuidBytes journal_id{};
  std::int64_t client_time_us = 0;  // 0: not sent
  std::vector<IncomingEntry> entries;
  std::vector<IncomingReport> reports;
};

struct EntryOutcome {
  engine::UuidBytes entry_uuid{};
  std::int64_t journal_sequence = 0;
  bool accepted = false;
  std::string reason;  // when rejected
};

struct SyncOutcome {
  std::vector<EntryOutcome> outcomes;
  std::vector<std::int64_t> acked_sequences;  // accepted this time or before
  std::int64_t last_acked_sequence = -1;
  int accepted = 0, rejected = 0;
  bool replayed = false;
  std::optional<std::int64_t> clock_divergence_us;
  std::vector<std::int64_t> exceptions_opened;  // ids newly opened by this batch
};

// The device is the authenticated caller's; the employee is the device's.
Result<SyncOutcome> apply_batch(core::Recorder& rec, const Config& cfg, const Device& device, const IncomingBatch& batch,
                                std::int64_t now_us);

// A manual entry by a supervisor or payroll: attestation manual, no
// journal, optionally correcting an existing entry (PL-4), which is then
// superseded and excluded from pairing. Refused when the entry it
// corrects, or the day's shifts, are approved or later.
struct ManualEntry {
  std::int64_t employee_id = 0;
  std::string kind;
  std::int64_t device_time_us = 0;
  std::string local_time, pay_code, note;  // zone and release are the server's (employee record, embedded database)
  std::string break_code;                  // optional, out punches only
  std::int64_t correction_of = 0;
  std::string reason;                      // free text, PL-4
  // Payroll reference rules: who is editing and why. edit_class is `payroll`
  // or `supervisor`; reason_code is a short code recorded with the edit. A
  // payroll edit of a timesheet that is held for overtime authorization is
  // refused as if the timesheet did not exist, because payroll cannot see it.
  std::string edit_class, reason_code, editor;
};
Result<TimeEntry> record_manual_entry(core::Recorder& rec, const Config& cfg, const ManualEntry& m, std::int64_t now_us);

// Rebuilds shifts of `employee_id` from `from_us` on (see above). Opens
// or resolves the pairing, long-shift and schedule exceptions as it goes.
Status repair_pairing(core::Recorder& rec, const Config& cfg, std::int64_t employee_id, std::int64_t from_us, std::int64_t now_us,
                      std::vector<std::int64_t>* exceptions_opened = nullptr);

}  // namespace archivum::punchline
