// Module-enforced invariants (docs/punchline-schema.md): for each, the
// engine accepts the row and the module's rule rejects it.
#include "archivum/core/module.h"
#include "archivum/punchline/localtime.h"
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
          Value::null(), Value::null(), Value::integer(0)};
}
Row audit_row(std::int64_t id) {
  return {Value::integer(id), Value::timestamp(1), Value::text("system"), Value::null(), Value::null(), Value::null(), Value::null(),
          Value::text("t"), Value::null(), Value::null(), Value::null(), Value::null()};
}
Row period(std::int64_t id, std::int64_t start, std::int64_t end, const char* state = "open") {
  return {Value::integer(id), Value::integer(start), Value::integer(end), Value::text("UTC"), Value::text("2024a"), Value::text(state),
          Value::null(), Value::null(), Value::null()};
}
punchline::TimeEntry entry(std::int64_t id, std::int64_t employee, std::int64_t device, const char* kind, std::int64_t at) {
  punchline::TimeEntry e;
  e.id = id;
  e.entry_uuid[0] = static_cast<std::byte>(id);
  e.employee_id = employee;
  e.device_id = device;
  e.journal_id = UuidBytes{};
  e.journal_sequence = id;
  e.kind = kind;
  e.device_time = at;
  e.local_time = "2024-03-04T08:00:00";
  e.site_zone = "UTC";
  e.tzdb_version = "2024a";
  e.receipt_time = at;
  e.attestation = "device";
  e.created_at = at;
  return e;
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
  REQUIRE_OK(w.value()->insert("audit_log", audit_row(1)));
  using punchline::rules::supervisor_assignment_valid;
  using punchline::rules::supervisor_at;
  // Self-supervision: the engine accepts, the module refuses. An inverted
  // range is the engine's own check since Stage 6 (same-row comparison).
  REQUIRE_OK(w.value()->insert("supervisor_assignments", {Value::integer(1), Value::integer(1), Value::integer(1), Value::timestamp(100),
                                                          Value::null(), Value::text("t"), Value::integer(1)}));
  CHECK(w.value()->insert("supervisor_assignments", {Value::integer(9), Value::integer(1), Value::integer(2), Value::timestamp(100),
                                                     Value::timestamp(50), Value::text("t"), Value::integer(1)}).code() == ErrorCode::Constraint);
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
  // Half-open: the boundary instant belongs to the later assignment only,
  // so no day has two supervisors (Stage 6 ruling).
  CHECK(supervisor_at(*w.value(), 1, 199).value().value() == 2);
  CHECK(supervisor_at(*w.value(), 1, 200).value().value() == 3);
  CHECK(supervisor_at(*w.value(), 1, 100).value().value() == 2);
  CHECK(!supervisor_at(*w.value(), 1, 99).value().has_value());
  CHECK(supervisor_at(*w.value(), 1, 1'000'000).value().value() == 3);  // open-ended
  CHECK(!supervisor_at(*w.value(), 2, 150).value().has_value());
}

ARCHIVUM_TEST(pl3_entry_period_contains_its_local_day) {
  MemVfs vfs;
  auto st = Store::open(vfs, "r/pl3.db");
  REQUIRE_OK(st.status());
  REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
  auto w = st.value()->begin_write();
  REQUIRE_OK(w.status());
  REQUIRE_OK(w.value()->insert("employees", employee(1)));
  REQUIRE_OK(w.value()->insert("devices", device(1, 1, false)));
  REQUIRE_OK(w.value()->insert("pay_periods", period(1, 19787, 19793)));  // 2024-03-04 .. 03-10
  REQUIRE_OK(w.value()->insert("pay_periods", period(2, 19794, 19800)));
  using punchline::rules::entry_period_valid;
  REQUIRE_OK(entry_period_valid(*w.value(), 19790, 1));
  REQUIRE_OK(entry_period_valid(*w.value(), 19790, 0));  // unassigned is allowed
  CHECK(entry_period_valid(*w.value(), 19790, 2).code() == ErrorCode::Constraint);
  CHECK(entry_period_valid(*w.value(), 19794, 1).code() == ErrorCode::Constraint);
  CHECK(entry_period_valid(*w.value(), 19790, 7).code() == ErrorCode::Constraint);
  // The engine accepts an entry in the wrong period; the module refuses it.
  punchline::TimeEntry e = entry(1, 1, 1, "in", 1'709'539'200'000'000);  // 2024-03-04T08:00:00 UTC, local day 19786+1
  e.period_id = 2;
  REQUIRE_OK(w.value()->insert("time_entries", e.to_row()));
  CHECK(entry_period_valid(*w.value(), punchline::parse_local_time(e.local_time).value().local_day, e.period_id).code() ==
        ErrorCode::Constraint);
  CHECK(punchline::period_for_day(*w.value(), 19790).value()->id == 1);
  CHECK(punchline::period_for_day(*w.value(), 19794).value()->id == 2);
  CHECK(!punchline::period_for_day(*w.value(), 19786).value().has_value());
  CHECK(!punchline::period_for_day(*w.value(), 19801).value().has_value());
}

ARCHIVUM_TEST(pl4_correction_is_of_the_same_employee_with_a_reason) {
  MemVfs vfs;
  auto st = Store::open(vfs, "r/pl4.db");
  REQUIRE_OK(st.status());
  REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
  auto w = st.value()->begin_write();
  REQUIRE_OK(w.status());
  REQUIRE_OK(w.value()->insert("employees", employee(1)));
  REQUIRE_OK(w.value()->insert("employees", employee(2)));
  REQUIRE_OK(w.value()->insert("devices", device(1, 1, false)));
  REQUIRE_OK(w.value()->insert("devices", device(2, 2, false)));
  REQUIRE_OK(w.value()->insert("time_entries", entry(1, 1, 1, "in", 1000).to_row()));
  using punchline::rules::correction_valid;
  REQUIRE_OK(correction_valid(*w.value(), 1, 1, "typo"));
  CHECK(correction_valid(*w.value(), 1, 2, "typo").code() == ErrorCode::Constraint);
  CHECK(correction_valid(*w.value(), 1, 1, "").code() == ErrorCode::Constraint);
  CHECK(correction_valid(*w.value(), 42, 1, "typo").code() == ErrorCode::Constraint);
  // The engine accepts a correction across employees with no reason; the module refuses it.
  punchline::TimeEntry c = entry(2, 2, 2, "in", 2000);
  c.correction_of = 1;
  c.attestation = "manual";
  REQUIRE_OK(w.value()->insert("time_entries", c.to_row()));
  CHECK(correction_valid(*w.value(), c.correction_of, c.employee_id, c.correction_reason).code() == ErrorCode::Constraint);
}

ARCHIVUM_TEST(pl5_device_attested_entry_needs_an_unrevoked_device) {
  MemVfs vfs;
  auto st = Store::open(vfs, "r/pl5.db");
  REQUIRE_OK(st.status());
  REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
  auto w = st.value()->begin_write();
  REQUIRE_OK(w.status());
  REQUIRE_OK(w.value()->insert("employees", employee(1)));
  REQUIRE_OK(w.value()->insert("devices", device(1, 1, false)));
  REQUIRE_OK(w.value()->insert("devices", device(2, 1, true)));  // revoked at 2
  using punchline::rules::device_attested_valid;
  REQUIRE_OK(device_attested_valid(*w.value(), 1, 100));
  REQUIRE_OK(device_attested_valid(*w.value(), 2, 1));  // received before the revocation
  CHECK(device_attested_valid(*w.value(), 2, 2).code() == ErrorCode::Constraint);
  CHECK(device_attested_valid(*w.value(), 2, 100).code() == ErrorCode::Constraint);
  CHECK(device_attested_valid(*w.value(), 3, 100).code() == ErrorCode::Constraint);
  // The engine accepts a device-attested entry from a revoked device; the module refuses it.
  REQUIRE_OK(w.value()->insert("time_entries", entry(1, 1, 2, "in", 100).to_row()));
  CHECK(device_attested_valid(*w.value(), 2, 100).code() == ErrorCode::Constraint);
}

ARCHIVUM_TEST(pl6_out_punch_closes_an_in_punch_of_the_same_employee_and_device) {
  punchline::Shift open;
  open.employee_id = 1;
  open.device_id = 1;
  open.in_entry_id = 1;
  open.in_time = 1000;
  using punchline::rules::pair_out;
  REQUIRE_OK(pair_out(open, entry(2, 1, 1, "out", 2000)));
  CHECK(pair_out(open, entry(2, 1, 1, "out", 1000)).code() == ErrorCode::Constraint);  // not after
  CHECK(pair_out(open, entry(2, 1, 1, "out", 500)).code() == ErrorCode::Constraint);
  CHECK(pair_out(open, entry(2, 2, 1, "out", 2000)).code() == ErrorCode::Constraint);  // another employee
  CHECK(pair_out(open, entry(2, 1, 2, "out", 2000)).code() == ErrorCode::Constraint);  // another device
  CHECK(pair_out(open, entry(2, 1, 1, "in", 2000)).code() == ErrorCode::Constraint);
  // The engine accepts a shift whose out is not after its in only through
  // the engine check shifts_times_ordered (Stage 6); the pairing rule
  // covers the cross-row part (same employee and device), which the engine
  // cannot: it accepts an out entry of another device in a shift.
  MemVfs vfs;
  auto st = Store::open(vfs, "r/pl6.db");
  REQUIRE_OK(st.status());
  REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
  auto w = st.value()->begin_write();
  REQUIRE_OK(w.status());
  REQUIRE_OK(w.value()->insert("employees", employee(1)));
  REQUIRE_OK(w.value()->insert("devices", device(1, 1, false)));
  REQUIRE_OK(w.value()->insert("devices", device(2, 1, true)));
  REQUIRE_OK(w.value()->insert("time_entries", entry(1, 1, 1, "in", 1000).to_row()));
  REQUIRE_OK(w.value()->insert("time_entries", entry(2, 1, 2, "out", 2000).to_row()));
  punchline::Shift sh = open;
  sh.id = 1;
  sh.out_entry_id = 2;
  sh.out_time = 2000;
  sh.duration_us = 1000;
  sh.local_day = 19786;
  REQUIRE_OK(w.value()->insert("shifts", sh.to_row()));
  CHECK(pair_out(open, entry(2, 1, 2, "out", 2000)).code() == ErrorCode::Constraint);
}

ARCHIVUM_TEST(pl7_transitions_in_order_by_the_right_standing_at_the_time) {
  MemVfs vfs;
  auto st = Store::open(vfs, "r/pl7.db");
  REQUIRE_OK(st.status());
  REQUIRE_OK(core::migrate_all(*st.value(), {&punchline::module()}).status());
  auto w = st.value()->begin_write();
  REQUIRE_OK(w.status());
  for (int i = 1; i <= 4; ++i) REQUIRE_OK(w.value()->insert("employees", employee(i)));
  REQUIRE_OK(w.value()->insert("audit_log", audit_row(1)));
  // 2 supervises 1 until 200; 3 from 200 on.
  REQUIRE_OK(w.value()->insert("supervisor_assignments", {Value::integer(1), Value::integer(1), Value::integer(2), Value::timestamp(0),
                                                          Value::timestamp(200), Value::text("t"), Value::integer(1)}));
  REQUIRE_OK(w.value()->insert("supervisor_assignments", {Value::integer(2), Value::integer(1), Value::integer(3), Value::timestamp(200),
                                                          Value::null(), Value::text("t"), Value::integer(1)}));
  using punchline::rules::Standing;
  using punchline::rules::transition_allowed;
  Standing self;
  self.employee_id = 1;
  Standing sup2;
  sup2.roles = {"supervisor"};
  sup2.employee_id = 2;
  Standing sup3 = sup2;
  sup3.employee_id = 3;
  Standing payroll;
  payroll.roles = {"payroll"};
  Standing admin;
  admin.roles = {"admin"};
  Standing stranger;
  stranger.employee_id = 4;
  // Order.
  CHECK(transition_allowed(*w.value(), 1, "recorded", "approved", sup2, 100).code() == ErrorCode::Constraint);
  CHECK(transition_allowed(*w.value(), 1, "approved", "submitted", payroll, 100).code() == ErrorCode::Constraint);
  CHECK(transition_allowed(*w.value(), 1, "locked", "", payroll, 100).code() == ErrorCode::Constraint);
  // Submit: the employee, their supervisor at the time, or payroll.
  REQUIRE_OK(transition_allowed(*w.value(), 1, "recorded", "submitted", self, 100));
  REQUIRE_OK(transition_allowed(*w.value(), 1, "recorded", "submitted", sup2, 100));
  CHECK(transition_allowed(*w.value(), 1, "recorded", "submitted", sup2, 250).code() == ErrorCode::Constraint);  // no longer theirs
  REQUIRE_OK(transition_allowed(*w.value(), 1, "recorded", "submitted", sup3, 250));
  REQUIRE_OK(transition_allowed(*w.value(), 1, "recorded", "submitted", payroll, 100));
  CHECK(transition_allowed(*w.value(), 1, "recorded", "submitted", stranger, 100).code() == ErrorCode::Constraint);
  // Approve: the supervisor in force at acted_at; payroll only with an override reason.
  REQUIRE_OK(transition_allowed(*w.value(), 1, "submitted", "approved", sup2, 199));
  CHECK(transition_allowed(*w.value(), 1, "submitted", "approved", sup2, 200).code() == ErrorCode::Constraint);
  REQUIRE_OK(transition_allowed(*w.value(), 1, "submitted", "approved", sup3, 200));
  CHECK(transition_allowed(*w.value(), 1, "submitted", "approved", self, 100).code() == ErrorCode::Constraint);
  CHECK(transition_allowed(*w.value(), 1, "submitted", "approved", payroll, 100).code() == ErrorCode::Constraint);
  Standing override = payroll;
  override.override_reason = "supervisor on leave past cutoff";
  REQUIRE_OK(transition_allowed(*w.value(), 1, "submitted", "approved", override, 100));
  // Release: payroll only. Lock: payroll or admin.
  REQUIRE_OK(transition_allowed(*w.value(), 1, "approved", "released", payroll, 100));
  CHECK(transition_allowed(*w.value(), 1, "approved", "released", sup3, 300).code() == ErrorCode::Constraint);
  CHECK(transition_allowed(*w.value(), 1, "approved", "released", admin, 300).code() == ErrorCode::Constraint);
  REQUIRE_OK(transition_allowed(*w.value(), 0, "released", "locked", payroll, 100));
  REQUIRE_OK(transition_allowed(*w.value(), 0, "released", "locked", admin, 100));
  CHECK(transition_allowed(*w.value(), 0, "released", "locked", sup3, 300).code() == ErrorCode::Constraint);
  // The engine accepts an out-of-order approvals row; the module refuses the transition.
  REQUIRE_OK(w.value()->insert("pay_periods", period(1, 1, 7)));
  REQUIRE_OK(w.value()->insert("approvals", {Value::integer(1), Value::integer(1), Value::integer(1), Value::text("recorded"),
                                             Value::text("released"), Value::null(), Value::null(), Value::null(), Value::timestamp(1),
                                             Value::null(), Value::integer(1)}));
  CHECK(transition_allowed(*w.value(), 1, "recorded", "released", payroll, 1).code() == ErrorCode::Constraint);
}

ARCHIVUM_TEST(local_time_arithmetic) {
  using punchline::days_from_civil;
  using punchline::format_local_day;
  using punchline::parse_local_time;
  using punchline::weekday_of_day;
  CHECK(days_from_civil(1970, 1, 1) == 0);
  CHECK(days_from_civil(2000, 3, 1) == 11017);
  CHECK(days_from_civil(2024, 3, 4) == 19786);
  CHECK(weekday_of_day(0) == 4);       // Thursday
  CHECK(weekday_of_day(19786) == 1);   // 2024-03-04 was a Monday
  CHECK(weekday_of_day(19785) == 0);   // Sunday
  CHECK(format_local_day(19786) == "2024-03-04");
  CHECK(format_local_day(0) == "1970-01-01");
  auto t = parse_local_time("2024-03-04T08:30:15");
  REQUIRE_OK(t.status());
  CHECK(t.value().local_day == 19786 && t.value().weekday == 1 && t.value().minute_of_day == 510);
  CHECK(parse_local_time("2024-02-30T08:30:15").status().code() == ErrorCode::InvalidArgument);
  CHECK(parse_local_time("2024-03-04 08:30:15").status().code() == ErrorCode::InvalidArgument);
  CHECK(parse_local_time("2024-03-04T24:00:00").status().code() == ErrorCode::InvalidArgument);
  REQUIRE_OK(parse_local_time("2024-02-29T23:59:59").status());
  CHECK(parse_local_time("2023-02-29T00:00:00").status().code() == ErrorCode::InvalidArgument);
}
