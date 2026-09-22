// The rollback export (Stage 6 rulings, item 3c): Archivum's entries for a
// pay period, rewritten as the batches the old FastAPI server ingests on
// `POST /api/v1/timesheets`, so that rolling a pilot device back to the
// old client means replaying this file into the old server through its
// own, uuid-idempotent ingest path. No schema coupling to the old store:
// its ingest endpoint is the contract, and the export is proven by
// posting it there (tests/contract/test_rollback.py).
//
// One completed shift becomes one old-store entry: uuid = the in punch's
// entry uuid (stable, so a replay is a no-op there too), employee_id =
// the employee number, name, company and cost centre from the employee
// record (server-owned; the old client asserted them), clock_in and
// clock_out from the punches, minutes computed, note from the in punch.
// Shifts are exported at every lifecycle state unless `released_only`,
// because a rollback happens mid-period and the old server has no
// approval step; the state is carried in `archivum_state` for the
// operator, a key the old server ignores (its models set extra="ignore").
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "archivum/engine/store.h"

namespace archivum::punchline {

struct RollbackExport {
  nlohmann::json batches;  // array of {client_version, device_id, submitted_at, entries: [...]}
  int shifts = 0;
  int open_shifts_skipped = 0;   // no out punch: the old store has no open shifts
  int employees_without_routing = 0;  // company or cost_center missing: exported with "" and counted
};

// `device_batch_size` matches the old client's batch size (100).
Result<RollbackExport> rollback_export(engine::Reader& reader, std::int64_t period_id, bool released_only,
                                       std::int64_t now_us, int device_batch_size = 100);

}  // namespace archivum::punchline
