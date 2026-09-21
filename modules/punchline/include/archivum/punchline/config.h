// Punchline's thresholds and cadences: configuration, never structure
// (punchline-updates.md section 11: assumptions are config). The server
// parses `modules.punchline` into this; every value is a default the Q8
// and section 11 answers will replace.
#pragma once

#include <cstdint>

namespace archivum::punchline {

struct Config {
  std::int64_t long_shift_hours = 16;                // a shift at or over this opens `long_shift`
  std::int64_t clock_divergence_tolerance_seconds = 300;  // device clock vs server clock at sync
  std::int64_t device_attested_threshold = 20;       // device-attested entries per employee per period before `device_attested_count`
  std::int64_t device_stale_seconds = 86400;         // no sync or heartbeat for this long opens `device_stale`
  std::int64_t schedule_tolerance_minutes = 30;      // punches this far outside the schedule are fine
  std::int64_t employee_id_rejection_threshold = 5;  // requests from one device asserting an employee id before `employee_id_asserted`
  std::int64_t monitor_interval_seconds = 60;        // freshness and cutoff checks
  std::int64_t max_batch_entries = 500;
};

}  // namespace archivum::punchline
