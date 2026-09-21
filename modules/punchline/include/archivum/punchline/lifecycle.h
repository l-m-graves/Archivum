// The approval lifecycle (punchline-updates.md section 4):
//   recorded -> submitted -> approved -> released -> locked
// per employee per pay period, every transition an `approvals` row under
// the caller's audit row, standing checked by PL-7 against the supervisor
// assignment in force at the time of the action. Nothing reaches payroll
// except by `release`, and the payroll export reads released and locked
// shifts only.
//
// A transition moves every entry and shift of the employee in the period
// that is in the predecessor state, and refuses to advance past an entry
// that is behind (a punch that arrived after submission stays `recorded`
// until resubmitted, so approval cannot skip it). Locking is per period:
// every entry must be released.
#pragma once

#include <cstdint>
#include <string>

#include "archivum/core/recorder.h"
#include "archivum/punchline/data.h"
#include "archivum/punchline/rules.h"

namespace archivum::punchline {

struct TransitionRequest {
  std::int64_t period_id = 0;
  std::int64_t employee_id = 0;
  std::string to_state;  // submitted | approved | released
  rules::Standing who;
  std::string acted_by_tid, acted_by_oid, acted_by_account;  // for the approvals row
  std::string note;
  std::int64_t now_us = 0;
};

struct TransitionResult {
  std::string from_state;
  int entries = 0;
  int shifts = 0;
  std::int64_t approval_id = 0;
  int exceptions_resolved = 0;
};

Result<TransitionResult> transition(core::Recorder& rec, const TransitionRequest& req);

// Locks the period: every entry in it must be released; then every
// entry and shift becomes locked, the period's state becomes locked, and
// one approvals row per employee with entries records it.
struct LockResult {
  int employees = 0;
  int entries = 0;
};
Result<LockResult> lock_period(core::Recorder& rec, std::int64_t period_id, const rules::Standing& who,
                               const std::string& acted_by_tid, const std::string& acted_by_oid,
                               const std::string& acted_by_account, std::int64_t now_us);

}  // namespace archivum::punchline
