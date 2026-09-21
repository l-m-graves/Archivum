// The exception queue's writer: opening is idempotent per (kind, employee,
// device, entry, period) while one is open, so a check that runs every
// sync or every monitor tick never duplicates an item. Every change goes
// through the caller's Recorder: the exception's audit trail is the
// action that produced it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "archivum/core/recorder.h"
#include "archivum/punchline/data.h"

namespace archivum::punchline {

struct ExceptionKey {
  std::string kind;
  std::int64_t employee_id = 0, device_id = 0, entry_id = 0, period_id = 0;  // 0: none
};

// Opens the exception unless an open one with the same key exists; returns
// the id either way and whether it was newly opened.
struct Opened {
  std::int64_t id = 0;
  bool created = false;
};
Result<Opened> open_exception(core::Recorder& rec, const ExceptionKey& key, const std::string& detail, std::int64_t now_us);
// Resolves (or dismisses) an open exception, naming who and the audit row.
Status close_exception(core::Recorder& rec, std::int64_t id, const std::string& new_state, const std::string& by,
                       std::int64_t now_us);
// Resolves every open exception matching the key (kind may be empty for
// any kind); returns how many.
Result<int> resolve_matching(core::Recorder& rec, const ExceptionKey& key, const std::string& by, std::int64_t now_us);
// Open exceptions, oldest first, optionally limited to employees in `employees`
// (empty: all) and to one kind (empty: all).
Result<std::vector<Exception>> open_exceptions(engine::Reader& r, const std::vector<std::int64_t>& employees,
                                               const std::string& kind = "");

}  // namespace archivum::punchline
