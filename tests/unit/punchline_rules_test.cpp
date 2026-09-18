// Module-enforced invariants (docs/punchline-schema.md): for each, the
// engine accepts the row and the module's rule rejects it.
#include "archivum/core/module.h"
#include "archivum/punchline/rules.h"
#include "archivum/punchline/schema.h"
#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

Row employee(std::int64_t id) {
  return {Value::integer(id), Value::text("E" + std::to_string(id)), Value::text("Name"), Value::null(), Value::null(),
          Value::null(), Value::boolean(true), Value::null(), Value::text("UTC"), Value::timestamp(1), Value::timestamp(1)};
}
Row device(std::int64_t id, std::int64_t employee, bool revoked) {
  UuidBytes u{};
  u[0] = static_cast<std::byte>(id);
  return {Value::integer(id), Value::uuid(u), Value::text("d"), Value::integer(employee), Value::blob({std::byte{1}}),
          Value::timestamp(1), Value::text("t"), revoked ? Value::timestamp(2) : Value::null(), Value::null(), Value::null(),
          Value::null()};
}

}  // namespace

ARCHIVUM_TEST(pl1_one_active_device_per_employee) {
  MemVfs vfs;
  auto st = Store::open(vfs, "r/pl1.db");
  REQUIRE_OK(st.status());
  REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
  auto w = st.value()->begin_write();
  REQUIRE_OK(w.status());
  REQUIRE_OK(w.value()->insert("employees", employee(1)));
  REQUIRE_OK(w.value()->insert("devices", device(1, 1, true)));  // revoked: does not count
  REQUIRE_OK(punchline::rules::one_active_device(*w.value(), 1));
  REQUIRE_OK(w.value()->insert("devices", device(2, 1, false)));
  // The engine accepts a second active device; the module refuses it.
  REQUIRE_OK(w.value()->insert("devices", device(3, 1, false)));
  CHECK(punchline::rules::one_active_device(*w.value(), 1).code() == ErrorCode::Constraint);
  REQUIRE_OK(w.value()->remove("devices", {Value::integer(3)}));
  CHECK(punchline::rules::one_active_device(*w.value(), 1).code() == ErrorCode::Constraint);  // device 2 is active
  REQUIRE_OK(punchline::rules::one_active_device(*w.value(), 1, /*except_device=*/2));
}

ARCHIVUM_TEST(pl2_supervisor_assignments_are_ordered_distinct_and_non_overlapping) {
  MemVfs vfs;
  auto st = Store::open(vfs, "r/pl2.db");
  REQUIRE_OK(st.status());
  REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
  auto w = st.value()->begin_write();
  REQUIRE_OK(w.status());
  for (int i = 1; i <= 3; ++i) REQUIRE_OK(w.value()->insert("employees", employee(i)));
  REQUIRE_OK(w.value()->insert("audit_log", {Value::integer(1), Value::timestamp(1), Value::text("system"), Value::null(), Value::null(),
                                             Value::null(), Value::null(), Value::text("t"), Value::null(), Value::null(), Value::null(),
                                             Value::null()}));
  using punchline::rules::supervisor_assignment_valid;
  // Self-supervision and an inverted range: the engine accepts, the module refuses.
  REQUIRE_OK(w.value()->insert("supervisor_assignments", {Value::integer(1), Value::integer(1), Value::integer(1), Value::timestamp(100),
                                                          Value::timestamp(50), Value::text("t"), Value::integer(1)}));
  CHECK(supervisor_assignment_valid(*w.value(), 1, 1, 100, 0).code() == ErrorCode::Constraint);
  CHECK(supervisor_assignment_valid(*w.value(), 1, 2, 100, 50).code() == ErrorCode::Constraint);
  REQUIRE_OK(w.value()->remove("supervisor_assignments", {Value::integer(1)}));
  // 2 supervises 1 from 100 to 200; 3 from 200 open-ended.
  REQUIRE_OK(supervisor_assignment_valid(*w.value(), 1, 2, 100, 200));
  REQUIRE_OK(w.value()->insert("supervisor_assignments", {Value::integer(2), Value::integer(1), Value::integer(2), Value::timestamp(100),
                                                          Value::timestamp(200), Value::text("t"), Value::integer(1)}));
  REQUIRE_OK(supervisor_assignment_valid(*w.value(), 1, 3, 200, 0));
  REQUIRE_OK(w.value()->insert("supervisor_assignments", {Value::integer(3), Value::integer(1), Value::integer(3), Value::timestamp(200),
                                                          Value::null(), Value::text("t"), Value::integer(1)}));
  // Overlaps: inside the first, straddling, and after the open-ended one.
  CHECK(supervisor_assignment_valid(*w.value(), 1, 3, 150, 160).code() == ErrorCode::Constraint);
  CHECK(supervisor_assignment_valid(*w.value(), 1, 2, 50, 150).code() == ErrorCode::Constraint);
  CHECK(supervisor_assignment_valid(*w.value(), 1, 2, 500, 0).code() == ErrorCode::Constraint);
  // Before the first is fine; editing an assignment ignores itself.
  REQUIRE_OK(supervisor_assignment_valid(*w.value(), 1, 3, 10, 100));
  REQUIRE_OK(supervisor_assignment_valid(*w.value(), 1, 2, 100, 200, /*except_assignment=*/2));
  // Another employee is independent.
  REQUIRE_OK(supervisor_assignment_valid(*w.value(), 2, 3, 150, 0));
}
