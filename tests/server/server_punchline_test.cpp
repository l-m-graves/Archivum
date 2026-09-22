// Stage 6: the Punchline module over HTTP. Sync with idempotent batches
// and per-entry outcomes, pairing and the exception queue, corrections,
// the approval lifecycle with its audit rows, employee self-service, the
// supervisor queue, the payroll export (released and locked only), the
// period audit report, the segregation-of-duties report, the employee_id
// tamper signal, and the freshness and cutoff monitor.
#include <vector>
#include <string>
#include <map>
#include <cstdio>
#include <chrono>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "archivum/core/schema.h"
#include "archivum/punchline/localtime.h"
#include "server_fixture.h"
#include "test.h"

using namespace archivum;
using namespace archivum::testing;

namespace {

constexpr std::int64_t kUs = 1'000'000;

ServerFixture& fixture() {
  static ServerFixture* f = nullptr;
  if (f == nullptr) {
    f = new ServerFixture();
    f->punchline_config = nlohmann::json{{"employee_id_rejection_threshold", 3},
                                         {"device_stale_seconds", 1},
                                         {"monitor_interval_seconds", 3600},
                                         {"device_attested_threshold", 4},
                                         {"long_shift_hours", 12}};
    Status s = f->start("1.3", 1);
    if (!s.ok()) std::fprintf(stderr, "fixture start failed: %s\n", s.to_string().c_str());
  }
  return *f;
}

std::string bearer_for(const char* oid, const char* sub) {
  TestIssuer::Claims c;
  c.oid = oid;
  c.subject = sub;
  return "Bearer " + fixture().issuer->mint(c);
}
std::string admin() { return bearer_for(ServerFixture::kAdminOid, "sub-0001"); }
std::string worker() { return bearer_for(ServerFixture::kEmployeeOid, "sub-0002"); }
std::string boss() { return bearer_for(ServerFixture::kSupervisorOid, "sub-0003"); }
std::string payroll() { return bearer_for(ServerFixture::kPayrollOid, "sub-0004"); }

nlohmann::json parse(const std::string& body) { return nlohmann::json::parse(body, nullptr, false); }

std::uint64_t audit_count(const std::string& action) {
  auto rd = fixture().app->store().begin_read();
  if (!rd.ok()) return 0;
  std::uint64_t n = 0;
  (void)rd.value()->scan("audit_log", "", std::nullopt, std::nullopt, false, [&](const engine::Row& r) {
    if (r[core::audit::kAction].as_text() == action) ++n;
    return true;
  });
  return n;
}

std::size_t open_exceptions(const std::string& kind) {
  auto rd = fixture().app->store().begin_read();
  std::size_t n = 0;
  (void)rd.value()->scan("exceptions", "", std::nullopt, std::nullopt, false, [&](const engine::Row& r) {
    if (r[1].as_text() == kind && r[2].as_text() == "open") ++n;
    return true;
  });
  return n;
}

std::string uid(int n) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012d", n);
  return buf;
}

// The week of 2024-03-04 (Monday) in UTC, so local wall clocks and instants agree.
constexpr std::int64_t kMonday = 1'709'510'400LL * kUs;
nlohmann::json punch(int n, int seq, const char* kind, int day, int hour, int minute = 0) {
  nlohmann::json e;
  e["entry_uuid"] = uid(n);
  e["journal_sequence"] = seq;
  e["kind"] = kind;
  e["device_time_us"] = kMonday + (day * 86400 + hour * 3600 + minute * 60) * kUs;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "T%02d:%02d:00", hour, minute);
  e["local_time"] = punchline::format_local_day(19786 + day) + buf;
  e["site_zone"] = "UTC";
  e["tzdb_version"] = "2024a";
  return e;
}
nlohmann::json batch(int n, std::initializer_list<nlohmann::json> entries) {
  nlohmann::json b;
  b["batch_uuid"] = uid(1000 + n);
  b["journal_id"] = uid(9000);
  b["entries"] = nlohmann::json::array();
  for (const auto& e : entries) b["entries"].push_back(e);
  return b;
}

// Set up by the first test: the worker's device credential and the period id.
std::string g_device_credential;
std::string g_device_uuid;
std::int64_t g_period = 0;

}  // namespace

ARCHIVUM_TEST(a_setup_people_roles_device_period_and_schedule) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  REQUIRE(f.wait_healthy());
  // Supervisor and payroll principals: employees with identities, plus their roles as data.
  auto sup = f.post("/api/v1/admin/employees",
                    nlohmann::json{{"employee_number", "S0001"}, {"display_name", "Sam Supervisor"}, {"site_zone", "UTC"},
                                   {"tid", ServerFixture::kTid}, {"oid", ServerFixture::kSupervisorOid}}
                        .dump(),
                    admin());
  REQUIRE_MSG(sup.status == 201, sup.body);
  REQUIRE(f.post("/api/v1/admin/roles", nlohmann::json{{"tid", ServerFixture::kTid}, {"oid", ServerFixture::kSupervisorOid}, {"role", "supervisor"}}.dump(),
                 admin()).status == 201);
  REQUIRE(f.post("/api/v1/admin/roles", nlohmann::json{{"tid", ServerFixture::kTid}, {"oid", ServerFixture::kPayrollOid}, {"role", "payroll"}}.dump(),
                 admin()).status == 201);
  // S0001 supervises E0001 (the seeded, mapped employee) from the epoch.
  REQUIRE(f.post("/api/v1/admin/supervisors",
                 nlohmann::json{{"employee_number", "E0001"}, {"supervisor_employee_number", "S0001"}, {"effective_from_us", 1}}.dump(), admin())
              .status == 201);
  // The worker's device.
  auto dev = f.post("/api/v1/admin/devices", nlohmann::json{{"employee_number", "E0001"}, {"name", "kiosk-1"}}.dump(), admin());
  REQUIRE_MSG(dev.status == 201, dev.body);
  g_device_credential = parse(dev.body)["credential"].get<std::string>();
  g_device_uuid = parse(dev.body)["device_uuid"].get<std::string>();
  // A pay period for the week of 2024-03-04 with cutoffs far in the future.
  auto period = f.post("/api/v1/admin/periods",
                       nlohmann::json{{"start_day", "2024-03-04"}, {"end_day", "2024-03-10"}, {"site_zone", "UTC"}, {"tzdb_version", "2024a"},
                                      {"submit_by_us", 4'000'000'000LL * kUs}, {"approve_by_us", 4'000'100'000LL * kUs}}
                           .dump(),
                       payroll());
  REQUIRE_MSG(period.status == 201, period.body);
  g_period = parse(period.body)["id"].get<std::int64_t>();
  CHECK(parse(period.body)["start_day"] == "2024-03-04");
  CHECK(f.post("/api/v1/admin/periods",
               nlohmann::json{{"start_day", "2024-03-08"}, {"end_day", "2024-03-14"}, {"site_zone", "UTC"}, {"tzdb_version", "2024a"}}.dump(),
               payroll()).status == 409);  // overlap
  CHECK(f.post("/api/v1/admin/periods",
               nlohmann::json{{"start_day", "2024-03-20"}, {"end_day", "2024-03-14"}, {"site_zone", "UTC"}, {"tzdb_version", "2024a"}}.dump(),
               payroll()).status == 409);  // inverted: the engine's same-row check
  // Monday to Friday, 09:00 to 17:00.
  for (int wd = 1; wd <= 5; ++wd) {
    REQUIRE(f.post("/api/v1/admin/schedules",
                   nlohmann::json{{"employee_number", "E0001"}, {"weekday", wd}, {"start_minute", 540}, {"end_minute", 1020},
                                  {"effective_from_day", "2024-01-01"}}
                       .dump(),
                   admin()).status == 201);
  }
  CHECK(f.post("/api/v1/admin/schedules",
               nlohmann::json{{"employee_number", "E0001"}, {"weekday", 1}, {"start_minute", 1020}, {"end_minute", 540},
                              {"effective_from_day", "2024-01-01"}}
                   .dump(),
               admin()).status == 409);  // end before start: engine check
  CHECK(audit_count("period.create") == 1 && audit_count("schedule.create") == 5 && audit_count("supervisor.assign") == 1);
}

ARCHIVUM_TEST(b_sync_is_idempotent_with_per_entry_outcomes) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  const std::string device = "Device " + g_device_credential;
  nlohmann::json b = batch(1, {punch(1, 1, "in", 0, 8, 55), punch(2, 2, "out", 0, 17, 5)});
  b["client_time_us"] = f.app->now_us() - 2 * kUs;
  auto r = f.post("/api/v1/device/sync", b.dump(), device);
  REQUIRE_MSG(r.status == 200, r.body);
  auto j = parse(r.body);
  CHECK(j["accepted"] == nlohmann::json::array({uid(1), uid(2)}));
  CHECK(j["rejected"].empty());
  CHECK(j["acked_sequences"] == nlohmann::json::array({1, 2}));
  CHECK(j["last_acked_sequence"] == 2 && j["replayed"] == false);
  CHECK(j.contains("clock_divergence_us") && j["exceptions_opened"] == 0);
  // Replay: same answer, nothing duplicated.
  auto again = f.post("/api/v1/device/sync", b.dump(), device);
  REQUIRE_MSG(again.status == 200, again.body);
  CHECK(parse(again.body)["accepted"] == j["accepted"] && parse(again.body)["replayed"] == true);
  auto sheet = f.get("/api/v1/timesheets/" + std::to_string(g_period) + "/me", worker());
  REQUIRE_MSG(sheet.status == 200, sheet.body);
  CHECK(parse(sheet.body)["entries"].size() == 2 && parse(sheet.body)["shifts"].size() == 1);
  CHECK(parse(sheet.body)["total_hours_hundredths"] == 816);  // 8h10m
  CHECK(parse(sheet.body)["entries"][0]["attestation"] == "device");
  CHECK(parse(sheet.body)["entries"][0]["employee_id"] == 1);  // from the enrollment, not the request
  // One bad entry does not sink the batch; a duplicate uuid inside one batch is one row;
  // a stray in, an out without in, a Saturday and a long day open queue items.
  nlohmann::json e_bad = punch(9, 9, "lunch", 1, 12);
  nlohmann::json e_shape = punch(10, 10, "in", 1, 12);
  e_shape["surprise"] = 1;
  nlohmann::json b2 = batch(2, {punch(3, 3, "in", 1, 9), punch(3, 3, "in", 1, 9), punch(4, 4, "in", 1, 13), punch(5, 5, "out", 1, 17),
                               e_bad, e_shape, punch(6, 6, "out", 2, 10), punch(7, 7, "in", 5, 10), punch(8, 8, "out", 5, 12),
                               punch(11, 11, "in", 3, 6), punch(12, 12, "out", 3, 23, 30)});
  b2["reports"] = nlohmann::json::array({nlohmann::json{{"kind", "retry_exhausted"}, {"detail", "5 attempts"}}});
  r = f.post("/api/v1/device/sync", b2.dump(), device);
  REQUIRE_MSG(r.status == 200, r.body);
  j = parse(r.body);
  CHECK_MSG(j["accepted"].size() == 9, j.dump());  // 11 sent, 2 rejected; uid 3 twice
  REQUIRE(j["rejected"].size() == 2);
  std::set<std::string> rejected_uuids;
  for (const auto& x : j["rejected"]) rejected_uuids.insert(x["uuid"].get<std::string>());
  CHECK(rejected_uuids == std::set<std::string>({uid(9), uid(10)}));
  CHECK(j["exceptions_opened"].get<int>() >= 5);
  CHECK(open_exceptions("unpaired_punch") == 2);
  CHECK(open_exceptions("non_scheduled_day") == 1);
  CHECK(open_exceptions("long_shift") == 1);
  CHECK(open_exceptions("outside_schedule") == 1);
  CHECK(open_exceptions("retry_exhausted") == 1);
  CHECK(open_exceptions("device_attested_count") == 1);  // 10 device-attested entries, threshold 4
  CHECK(audit_count("sync.batch") == 3);
  // Nothing but a device credential may sync; a bearer is refused.
  CHECK(f.post("/api/v1/device/sync", b.dump(), worker()).status == 403);
  // A batch over the limit is 413 before anything is looked at.
  nlohmann::json huge = batch(3, {});
  for (int i = 0; i < 501; ++i) huge["entries"].push_back(punch(100 + i, 100 + i, "in", 0, 1));
  CHECK(f.post("/api/v1/device/sync", huge.dump(), device).status == 413);
  // The old client's server-owned values are refused, per entry, by name:
  // name, company and cost centre come from the employee record and
  // minutes from the punches (Stage 6 rulings, item 3b).
  for (const char* key : {"employee_name", "company", "cost_center", "minutes"}) {
    nlohmann::json owned = batch(30, {punch(60, 60, "in", 0, 9)});
    owned["entries"][0][key] = key == std::string("minutes") ? nlohmann::json(480) : nlohmann::json("asserted");
    auto rr = f.post("/api/v1/device/sync", owned.dump(), device);
    REQUIRE_MSG(rr.status == 200, rr.body);
    const auto jj = parse(rr.body);
    CHECK_MSG(jj["accepted"].empty() && jj["rejected"].size() == 1, key << ": " << rr.body);
    CHECK_MSG(jj["rejected"][0]["reason"].get<std::string>().find(key) != std::string::npos, rr.body);
  }
  CHECK(open_exceptions("entry_rejected") == 1);
  // The note is free text: never by value in the feed or the audit log.
  // Every key the contract names, in one batch (docs/punchline-module.md, "Sync").
  nlohmann::json b4 = batch(4, {punch(13, 13, "in", 4, 9)});
  b4["client_time_us"] = f.app->now_us();
  b4["entries"][0]["note"] = "CANARY-NOTE-7f3a";
  b4["entries"][0]["pay_code"] = "regular";
  b4["reports"] = nlohmann::json::array({nlohmann::json{{"kind", "journal_recovery"}, {"detail", "12 bytes discarded"}}});
  auto noted = f.post("/api/v1/device/sync", b4.dump(), device);
  REQUIRE_MSG(noted.status == 200, noted.body);
  auto rd = f.app->store().begin_read();
  for (const char* table : {"change_feed", "audit_log"}) {
    auto rows = rd.value()->scan_all(table);
    REQUIRE_OK(rows.status());
    for (const engine::Row& row : rows.value()) {
      for (const engine::Value& v : row) {
        if (v.kind() == engine::Value::Kind::Text) CHECK_MSG(v.as_text().find("CANARY-NOTE") == std::string::npos, table);
      }
    }
  }
}

ARCHIVUM_TEST(c_exception_queue_scopes_and_closure) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  auto mine = f.get("/api/v1/exceptions", worker());
  REQUIRE_MSG(mine.status == 200, mine.body);
  CHECK(parse(mine.body)["count"].get<int>() >= 5);
  auto theirs = f.get("/api/v1/exceptions", boss());
  REQUIRE_MSG(theirs.status == 200, theirs.body);
  CHECK(parse(theirs.body)["count"] == parse(mine.body)["count"]);
  auto all = f.get("/api/v1/exceptions?kind=unpaired_punch", payroll());
  REQUIRE_MSG(all.status == 200, all.body);
  // The stray Tuesday in, the Wednesday out, and the Friday in still open
  // (2024 punches against a 2026 clock: open past the threshold).
  const auto unpaired_before = parse(all.body)["count"].get<int>();
  REQUIRE(unpaired_before == 3);
  const std::int64_t id = parse(all.body)["exceptions"][0]["id"].get<std::int64_t>();
  // The employee cannot close their own item; their supervisor can; twice is a conflict.
  CHECK(f.post("/api/v1/exceptions/" + std::to_string(id) + "/resolve", "{}", worker()).status == 403);
  auto closed = f.post("/api/v1/exceptions/" + std::to_string(id) + "/dismiss", nlohmann::json{{"note", "double tap"}}.dump(), boss());
  CHECK_MSG(closed.status == 200, closed.body);
  CHECK(f.post("/api/v1/exceptions/" + std::to_string(id) + "/resolve", "{}", boss()).status == 409);
  CHECK(audit_count("exception.dismiss") == 1);
  CHECK(open_exceptions("unpaired_punch") == 2);
  // Self-service: the approver's name and the flag action.
  auto me = f.get("/api/v1/me", worker());
  REQUIRE_MSG(me.status == 200, me.body);
  CHECK(parse(me.body)["approver"]["display_name"] == "Sam Supervisor");
  CHECK(parse(me.body)["clocked_in"] == true);  // the Thursday in has no out yet
  auto flag = f.post("/api/v1/me/flag", nlohmann::json{{"note", "my approver changed teams"}}.dump(), worker());
  CHECK_MSG(flag.status == 201, flag.body);
  CHECK(f.post("/api/v1/me/flag", nlohmann::json{{"note", "again"}}.dump(), worker()).status == 200);  // one open ticket
  CHECK(open_exceptions("approver_flag") == 1);
  CHECK(audit_count("approver.flag") == 2);
}

ARCHIVUM_TEST(d_correction_by_the_supervisor_supersedes_the_entry) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  // The stray Tuesday 09:00 in (uid 3) is corrected by a manual entry; the
  // worker may not, a stranger may not, the supervisor may.
  auto rd = f.app->store().begin_read();
  std::int64_t stray = 0;
  (void)rd.value()->scan("time_entries", "", std::nullopt, std::nullopt, false, [&](const engine::Row& r) {
    if (r[1].to_string() == uid(3)) stray = r[0].as_int64();
    return true;
  });
  rd.value().reset();
  REQUIRE(stray != 0);
  nlohmann::json m{{"employee_number", "E0001"}, {"kind", "in"},   {"device_time_us", kMonday + (86400 + 9 * 3600 + 5 * 60) * kUs},
                   {"local_time", "2024-03-05T09:05:00"}, {"site_zone", "UTC"}, {"tzdb_version", "2024a"},
                   {"correction_of", stray}, {"reason", "double punch at the door"}};
  CHECK(f.post("/api/v1/entries/manual", m.dump(), worker()).status == 403);
  auto r = f.post("/api/v1/entries/manual", m.dump(), boss());
  REQUIRE_MSG(r.status == 201, r.body);
  CHECK(parse(r.body)["attestation"] == "manual" && parse(r.body)["correction_of"] == stray);
  CHECK(audit_count("entry.correct") == 1);
  nlohmann::json no_reason = m;
  no_reason["reason"] = "";
  CHECK(f.post("/api/v1/entries/manual", no_reason.dump(), boss()).status == 400);
  CHECK(f.post("/api/v1/entries/manual", m.dump(), boss()).status == 409);  // already corrected
}

ARCHIVUM_TEST(e_lifecycle_over_http_reaches_payroll_only_by_release) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  const std::string p = "/api/v1/periods/" + std::to_string(g_period);
  // Nothing released yet: the payroll export withholds everything.
  auto before = f.get("/api/v1/payroll/periods/" + std::to_string(g_period), payroll());
  REQUIRE_MSG(before.status == 200, before.body);
  CHECK(parse(before.body)["employees"].empty() && parse(before.body)["shifts_withheld_not_released"].get<int>() > 0);
  CHECK(f.get("/api/v1/payroll/periods/" + std::to_string(g_period), boss()).status == 403);
  // Out of order and without standing: refused, no audit row.
  CHECK(f.post(p + "/approve", nlohmann::json{{"employee_number", "E0001"}}.dump(), boss()).status == 409);
  CHECK(f.post(p + "/submit", nlohmann::json{{"employee_number", "E0001"}}.dump(), payroll()).status == 200);  // payroll may submit
  CHECK(f.post(p + "/submit", "{}", worker()).status == 409);  // nothing left to submit
  CHECK(f.post(p + "/approve", nlohmann::json{{"employee_number", "E0001"}}.dump(), worker()).status == 409);
  CHECK(f.post(p + "/approve", nlohmann::json{{"employee_number", "E0001"}}.dump(), payroll()).status == 409);  // needs an override reason
  auto approved = f.post(p + "/approve", nlohmann::json{{"employee_number", "E0001"}, {"note", "reviewed"}}.dump(), boss());
  REQUIRE_MSG(approved.status == 200, approved.body);
  CHECK(parse(approved.body)["from_state"] == "submitted" && parse(approved.body)["exceptions_resolved"].get<int>() >= 1);
  CHECK(open_exceptions("device_attested_count") == 0);  // resolved through approval
  auto queue = f.get("/api/v1/supervisor/periods/" + std::to_string(g_period), boss());
  REQUIRE_MSG(queue.status == 200, queue.body);
  CHECK(parse(queue.body)["reports"].size() == 1 && parse(queue.body)["reports"][0]["awaiting_approval"] == false);
  CHECK(f.get("/api/v1/supervisor/periods/" + std::to_string(g_period), worker()).status == 403);
  // A late punch after approval is accepted and flagged; approved shifts stay.
  const std::string device = "Device " + g_device_credential;
  auto late = f.post("/api/v1/device/sync", batch(5, {punch(20, 20, "out", 4, 18)}).dump(), device);
  REQUIRE_MSG(late.status == 200, late.body);
  CHECK(parse(late.body)["accepted"].size() == 1);
  // The supervisor sees it in their queue as a late punch, not merely as unpaired.
  auto late_items = f.get("/api/v1/exceptions?kind=late_punch", boss());
  REQUIRE_MSG(late_items.status == 200, late_items.body);
  CHECK(parse(late_items.body)["count"] == 1);
  CHECK(parse(late_items.body)["exceptions"][0]["period_id"] == g_period);
  CHECK(f.post(p + "/release", nlohmann::json{{"employee_number", "E0001"}}.dump(), payroll()).status == 409);  // the late entry blocks
  CHECK(f.post(p + "/submit", nlohmann::json{{"employee_number", "E0001"}}.dump(), payroll()).status == 200);
  auto override = f.post(p + "/approve", nlohmann::json{{"employee_number", "E0001"}, {"override_reason", "supervisor out; cutoff"}}.dump(),
                         payroll());
  REQUIRE_MSG(override.status == 200, override.body);
  CHECK(open_exceptions("late_punch") == 0);  // closed by the re-approval
  auto released = f.post(p + "/release", nlohmann::json{{"employee_number", "E0001"}}.dump(), payroll());
  REQUIRE_MSG(released.status == 200, released.body);
  CHECK(f.post(p + "/release", nlohmann::json{{"employee_number", "E0001"}}.dump(), boss()).status == 409);
  // Now payroll sees hours, JSON and CSV.
  auto after = f.get("/api/v1/payroll/periods/" + std::to_string(g_period), payroll());
  REQUIRE_MSG(after.status == 200, after.body);
  REQUIRE(parse(after.body)["employees"].size() == 1);
  CHECK(parse(after.body)["employees"][0]["hours_hundredths_by_pay_code"]["regular"].get<int>() > 3000);
  auto csv = f.get("/api/v1/payroll/periods/" + std::to_string(g_period) + "?format=csv", payroll());
  REQUIRE(csv.status == 200);
  CHECK(csv.body.rfind("employee_number,display_name,pay_code,hours_hundredths,shifts\n", 0) == 0);
  CHECK(csv.body.find("E0001,Mapped Employee,regular,") != std::string::npos);
  // Lock: payroll, then nothing moves and the period reads locked.
  CHECK(f.post(p + "/lock", "{}", boss()).status == 409);
  auto locked = f.post(p + "/lock", "{}", payroll());
  REQUIRE_MSG(locked.status == 200, locked.body);
  CHECK(parse(locked.body)["employees"] == 1);
  CHECK(f.post(p + "/submit", nlohmann::json{{"employee_number", "E0001"}}.dump(), payroll()).status == 409);
  CHECK(parse(f.get("/api/v1/admin/periods", payroll()).body)["periods"][0]["state"] == "locked");
  // Every transition is an audit row with an approvals row; the audit report lists them with actors.
  CHECK(audit_count("period.submit") == 2 && audit_count("period.approve") == 2 && audit_count("period.release") == 1 &&
        audit_count("period.lock") == 1);
  auto report = f.get("/api/v1/payroll/periods/" + std::to_string(g_period) + "/audit", payroll());
  REQUIRE_MSG(report.status == 200, report.body);
  auto rep = parse(report.body);
  REQUIRE(rep["transitions"].size() == 6);
  int overrides = 0;
  for (const auto& t : rep["transitions"]) {
    CHECK(t.contains("actor_kind") && t.contains("at_us") && t.contains("audit_id"));
    if (t["override"] == true) ++overrides;
  }
  CHECK(overrides == 1);
  CHECK(rep["manual_entries"].size() == 1);
  CHECK(!rep["exceptions"].empty());
  CHECK(f.get("/api/v1/payroll/periods/" + std::to_string(g_period) + "/audit", worker()).status == 403);
  // A punch for the locked period is accepted, unassigned, flagged past_cutoff.
  auto after_lock = f.post("/api/v1/device/sync", batch(6, {punch(21, 21, "in", 2, 9)}).dump(), device);
  REQUIRE_MSG(after_lock.status == 200, after_lock.body);
  CHECK(open_exceptions("past_cutoff") == 1);
}

ARCHIVUM_TEST(f_segregation_of_duties_report) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  auto none = f.get("/api/v1/admin/roles/conflicts", admin());
  REQUIRE_MSG(none.status == 200, none.body);
  CHECK(parse(none.body)["conflicts"].empty());
  auto grant = f.post("/api/v1/admin/roles", nlohmann::json{{"tid", ServerFixture::kTid}, {"oid", ServerFixture::kSupervisorOid}, {"role", "payroll"}}.dump(),
                      admin());
  REQUIRE(grant.status == 201);
  auto one = f.get("/api/v1/admin/roles/conflicts", payroll());
  REQUIRE_MSG(one.status == 200, one.body);
  REQUIRE(parse(one.body)["conflicts"].size() == 1);
  CHECK(parse(one.body)["conflicts"][0]["oid"] == ServerFixture::kSupervisorOid);
  CHECK(parse(one.body)["conflicts"][0]["employee"]["employee_number"] == "S0001");
  REQUIRE(f.post("/api/v1/admin/roles/" + std::to_string(parse(grant.body)["id"].get<std::int64_t>()) + "/revoke", "{}", admin()).status == 200);
  CHECK(parse(f.get("/api/v1/admin/roles/conflicts", admin()).body)["conflicts"].empty());
  CHECK(f.get("/api/v1/admin/roles/conflicts", worker()).status == 403);
}

ARCHIVUM_TEST(g_employee_id_from_a_device_is_a_tamper_signal) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  const std::string device = "Device " + g_device_credential;
  const auto tamper_before = audit_count("device.tamper_signal");
  nlohmann::json b = batch(7, {punch(30, 30, "in", 4, 9)});
  b["entries"][0]["employee_id"] = "E9999";
  for (int i = 0; i < 3; ++i) {
    auto r = f.post("/api/v1/device/sync", b.dump(), device);
    CHECK_MSG(r.status == 400, r.body);
    CHECK(parse(r.body)["message"].get<std::string>().find("employee_id") != std::string::npos);
  }
  const std::string log = f.log_text();
  CHECK(log.find("\"event\":\"request.employee_id_asserted\"") != std::string::npos);
  CHECK(log.find("\"device\":\"" + g_device_uuid + "\"") != std::string::npos);
  CHECK(log.find("\"key\":\"entries.employee_id\"") != std::string::npos);
  CHECK(log.find("\"event\":\"device.tamper_signal\"") != std::string::npos);
  CHECK(audit_count("device.tamper_signal") == tamper_before + 1);
  CHECK(open_exceptions("employee_id_asserted") == 1);
  auto devices = f.get("/api/v1/admin/devices", admin());
  REQUIRE(devices.status == 200);
  CHECK(parse(devices.body)["devices"][0]["employee_id_rejections"].get<int>() >= 3);
  // Nothing was recorded from those requests.
  CHECK(audit_count("sync.batch") == 10);  // 6 accepted batches, 4 refused-key batches (each still one audited request)
  // The same key from a principal is logged with the caller, not counted.
  auto r = f.post("/api/v1/admin/employees", nlohmann::json{{"employee_id", 1}}.dump(), admin());
  CHECK(r.status == 400);
  CHECK(f.log_text().find("\"caller\":\"principal ") != std::string::npos);
}

ARCHIVUM_TEST(h_monitor_flags_stale_devices_and_cutoffs) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  const auto& mod = f.app->modules().at(0);
  REQUIRE(mod.run_once != nullptr);
  // The device synced moments ago; with a 1 s threshold it is stale after a pause.
  auto first = mod.run_once(*f.app);
  REQUIRE_OK(first.status());
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  auto second = mod.run_once(*f.app);
  REQUIRE_OK(second.status());
  CHECK(first.value() + second.value() >= 1);
  CHECK(open_exceptions("device_stale") == 1);
  CHECK(f.log_text().find("\"event\":\"device.stale\"") != std::string::npos);
  CHECK(mod.run_once(*f.app).value() == 0);  // idempotent while open
  // A heartbeat restores freshness and resolves the item, through the recorder.
  auto hb = f.post("/api/v1/device/heartbeat", "{}", "Device " + g_device_credential);
  REQUIRE_MSG(hb.status == 200, hb.body);
  CHECK(open_exceptions("device_stale") == 0);
  CHECK(audit_count("device.heartbeat") == 1);
  // Escalation: a period past its cutoffs with entries behind.
  auto period = f.post("/api/v1/admin/periods",
                       nlohmann::json{{"start_day", "2024-03-11"}, {"end_day", "2024-03-17"}, {"site_zone", "UTC"}, {"tzdb_version", "2024a"},
                                      {"submit_by_us", 1}, {"approve_by_us", 2}}
                           .dump(),
                       payroll());
  REQUIRE_MSG(period.status == 201, period.body);
  auto sync = f.post("/api/v1/device/sync", batch(8, {punch(40, 40, "in", 7, 9), punch(41, 41, "out", 7, 17)}).dump(), "Device " + g_device_credential);
  REQUIRE_MSG(sync.status == 200, sync.body);
  auto third = mod.run_once(*f.app);
  REQUIRE_OK(third.status());
  CHECK(third.value() >= 1);
  CHECK(open_exceptions("past_cutoff") == 2);  // the locked-period punch from before, and this one
  CHECK(f.log_text().find("\"event\":\"period.past_cutoff\"") != std::string::npos);
  // Payroll resolves it by approving with an override; the item closes with the approval.
  const std::string p = "/api/v1/periods/" + std::to_string(parse(period.body)["id"].get<std::int64_t>());
  REQUIRE(f.post(p + "/submit", nlohmann::json{{"employee_number", "E0001"}}.dump(), payroll()).status == 200);
  auto approved = f.post(p + "/approve", nlohmann::json{{"employee_number", "E0001"}, {"override_reason", "past cutoff"}}.dump(), payroll());
  REQUIRE_MSG(approved.status == 200, approved.body);
  CHECK(open_exceptions("past_cutoff") == 1);
}

ARCHIVUM_TEST(zz_shutdown_fixture) {
  auto& f = fixture();
  f.stop();
  CHECK(f.finished.load());
}
