// Module-enforced invariants: rules the engine's constraints cannot
// express (docs/punchline-schema.md, "Enforced by the module"). Each is a
// function the request handlers call before mutating, and each has a test
// asserting that the engine accepts the bad row and the module rejects it.
// Every one returns ErrorCode::Constraint with the rule's name.
#pragma once

#include <cstdint>

#include "archivum/engine/store.h"

namespace archivum::punchline::rules {

// PL-1: at most one unrevoked device per employee. `except_device` is the
// device being updated, if any.
Status one_active_device(engine::Reader& reader, std::int64_t employee_id, std::int64_t except_device = 0);

// PL-2: a supervisor assignment names two different employees, its range
// is ordered (effective_to, when present, after effective_from), and it
// does not overlap another assignment of the same employee.
Status supervisor_assignment_valid(engine::Reader& reader, std::int64_t employee_id, std::int64_t supervisor_employee_id,
                                   std::int64_t effective_from, std::int64_t effective_to /*0: open*/,
                                   std::int64_t except_assignment = 0);

// PL-3: a time entry's period, when set, must contain its device_time
// (site-local days); PL-4: a correction must be of an entry of the same
// employee; PL-5: a device-attested entry must come from a device that was
// unrevoked at receipt time. Implemented in Stage 6 with the sync endpoint.

}  // namespace archivum::punchline::rules
