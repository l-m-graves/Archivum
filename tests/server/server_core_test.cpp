// Stage 5: the server core. Identity with standing (roles as data, the
// nullable-oid rule), device enrollment and the per-device credential,
// break-glass accounts, the off-host archive check in the health endpoint,
// audit rows inside the business transaction, structured logs, and the
// contract test: employee_id in any request body is rejected on every
// endpoint.
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "archivum/core/accounts.h"
#include "archivum/core/schema.h"
#include "server_fixture.h"
#include "test.h"

using namespace archivum;
using namespace archivum::testing;

namespace {

ServerFixture& fixture() {
  static ServerFixture* f = nullptr;
  if (f == nullptr) {
    f = new ServerFixture();
    Status s = f->start("1.3", 1);
    if (!s.ok()) std::fprintf(stderr, "fixture start failed: %s\n", s.to_string().c_str());
  }
  return *f;
}

std::string admin_bearer() {
  TestIssuer::Claims c;
  c.oid = ServerFixture::kAdminOid;
  return "Bearer " + fixture().issuer->mint(c);
}
std::string employee_bearer() {
  TestIssuer::Claims c;
  c.oid = ServerFixture::kEmployeeOid;
  c.subject = "sub-0002";
  return "Bearer " + fixture().issuer->mint(c);
}
std::string stranger_bearer() {
  TestIssuer::Claims c;
  c.oid = "8f1c2b2e-0000-4000-8000-0000000000ff";
  c.subject = "sub-00ff";
  return "Bearer " + fixture().issuer->mint(c);
}

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

std::string base64(const std::string& in) {
  static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -6;
  for (char ch : in) {
    const unsigned char c = static_cast<unsigned char>(ch);
    val = (val << 8) + c;
    bits += 8;
    while (bits >= 0) {
      out += a[(val >> bits) & 0x3F];
      bits -= 6;
    }
  }
  if (bits > -6) out += a[((val << 8) >> (bits + 8)) & 0x3F];
  while (out.size() % 4) out += '=';
  return out;
}

}  // namespace

ARCHIVUM_TEST(identity_has_standing_only_through_roles_or_an_employee_row) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  auto admin = f.get("/api/v1/whoami", admin_bearer());
  REQUIRE_MSG(admin.status == 200, admin.body);
  auto j = parse(admin.body);
  CHECK(j["roles"] == nlohmann::json::array({"admin"}));
  CHECK(!j.contains("employee_id"));
  auto emp = f.get("/api/v1/whoami", employee_bearer());
  REQUIRE_MSG(emp.status == 200, emp.body);
  j = parse(emp.body);
  CHECK(j["roles"].empty());
  CHECK(j["employee_id"] == 1);
  // A valid token for an identity nobody mapped: 403, audited, alerted.
  // Email never authorizes: the stranger carries the same email as everyone.
  const auto before = audit_count("auth.unknown_principal");
  auto stranger = f.get("/api/v1/whoami", stranger_bearer());
  CHECK_MSG(stranger.status == 403, stranger.body);
  CHECK(audit_count("auth.unknown_principal") == before + 1);
  CHECK(f.log_text().find("\"event\":\"auth.unknown_principal\"") != std::string::npos);
  CHECK(f.log_text().find("\"level\":\"alert\"") != std::string::npos);
  // Roles are data: grant the stranger payroll and they have standing.
  auto grant = f.post("/api/v1/admin/roles",
                      nlohmann::json{{"tid", ServerFixture::kTid}, {"oid", "8f1c2b2e-0000-4000-8000-0000000000ff"}, {"role", "payroll"}}.dump(),
                      admin_bearer());
  REQUIRE_MSG(grant.status == 201, grant.body);
  const std::int64_t grant_id = parse(grant.body)["id"].get<std::int64_t>();
  CHECK(parse(f.get("/api/v1/whoami", stranger_bearer()).body)["roles"] == nlohmann::json::array({"payroll"}));
  // The employee cannot grant roles; the stranger (payroll) cannot either.
  CHECK(f.post("/api/v1/admin/roles", nlohmann::json{{"tid", "t"}, {"oid", "o"}, {"role", "admin"}}.dump(), employee_bearer()).status == 403);
  CHECK(f.post("/api/v1/admin/roles", nlohmann::json{{"tid", "t"}, {"oid", "o"}, {"role", "admin"}}.dump(), stranger_bearer()).status == 403);
  // Revoke: back to no standing.
  auto revoke = f.post("/api/v1/admin/roles/" + std::to_string(grant_id) + "/revoke", "", admin_bearer());
  REQUIRE_MSG(revoke.status == 200, revoke.body);
  CHECK(f.get("/api/v1/whoami", stranger_bearer()).status == 403);
  CHECK(audit_count("role.grant") == 1 && audit_count("role.revoke") == 1);
}

ARCHIVUM_TEST(device_enrollment_credential_heartbeat_and_revocation) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  // An employee without an identity (oid nullable).
  auto created = f.post("/api/v1/admin/employees",
                        nlohmann::json{{"employee_number", "E0002"}, {"display_name", "Kiosk User"}, {"site_zone", "America/Los_Angeles"}}.dump(),
                        admin_bearer());
  REQUIRE_MSG(created.status == 201, created.body);
  CHECK(parse(created.body)["oid"] == "");
  // Enroll.
  auto enrolled = f.post("/api/v1/admin/devices", nlohmann::json{{"employee_number", "E0002"}, {"name", "kiosk-1"}}.dump(), admin_bearer());
  REQUIRE_MSG(enrolled.status == 201, enrolled.body);
  auto ej = parse(enrolled.body);
  const std::string credential = ej["credential"].get<std::string>();
  const std::string uuid = ej["device_uuid"].get<std::string>();
  CHECK(credential.rfind(uuid + ":", 0) == 0);
  // PL-1: a second active device for the same employee is refused.
  auto second = f.post("/api/v1/admin/devices", nlohmann::json{{"employee_number", "E0002"}, {"name", "kiosk-2"}}.dump(), admin_bearer());
  CHECK_MSG(second.status == 409, second.body);
  CHECK(parse(second.body)["message"].get<std::string>().find("PL-1") != std::string::npos);
  // The credential is never stored: the list shows no secret.
  auto list = f.get("/api/v1/admin/devices", admin_bearer());
  REQUIRE(list.status == 200);
  CHECK(list.body.find(credential.substr(uuid.size() + 1)) == std::string::npos);
  // Heartbeat with the device credential: the employee is the enrollment's.
  auto hb = f.post("/api/v1/device/heartbeat", nlohmann::json{{"client_time_us", 5}}.dump(), "Device " + credential);
  REQUIRE_MSG(hb.status == 200, hb.body);
  CHECK(parse(hb.body)["employee"]["employee_number"] == "E0002");
  CHECK(parse(hb.body).contains("clock_divergence_us"));
  // Wrong secret, unknown device, bearer on a device route.
  CHECK(f.post("/api/v1/device/heartbeat", "{}", "Device " + uuid + ":wrong").status == 401);
  CHECK(f.post("/api/v1/device/heartbeat", "{}", "Device 00000000-0000-4000-8000-000000000000:x").status == 401);
  CHECK(f.post("/api/v1/device/heartbeat", "{}", admin_bearer()).status == 403);
  CHECK(audit_count("auth.device_bad_credential") == 1);
  // Revoke: the credential stops working, with a 403 and an audit row.
  auto revoked = f.post("/api/v1/admin/devices/" + uuid + "/revoke", nlohmann::json{{"reason", "lost"}}.dump(), admin_bearer());
  REQUIRE_MSG(revoked.status == 200, revoked.body);
  CHECK(parse(revoked.body)["revoked"] == true);
  CHECK(f.post("/api/v1/device/heartbeat", "{}", "Device " + credential).status == 403);
  CHECK(audit_count("auth.device_revoked") == 1);
  CHECK(f.post("/api/v1/admin/devices/" + uuid + "/revoke", nlohmann::json{{"reason", "again"}}.dump(), admin_bearer()).status == 409);
  // A replacement device can now be enrolled.
  CHECK(f.post("/api/v1/admin/devices", nlohmann::json{{"employee_number", "E0002"}, {"name", "kiosk-2"}}.dump(), admin_bearer()).status == 201);
  // Every mutation carries its audit row and feed images in the same transaction.
  CHECK(audit_count("device.enroll") == 2 && audit_count("device.revoke") == 1 && audit_count("employee.create") == 1);
  auto rd = f.app->store().begin_read();
  REQUIRE_OK(rd.status());
  auto feed = rd.value()->scan_all(core::kChangeFeed, "change_feed_table", engine::Bound{{engine::Value::text("devices")}},
                                   engine::Bound{{engine::Value::text("devices")}});
  REQUIRE_OK(feed.status());
  CHECK(feed.value().size() == 3);  // enroll, revoke, enroll; the heartbeat's last_seen is not audited
  for (const auto& row : feed.value()) {
    auto img = nlohmann::json::parse(row[core::feed::kAfter].as_text());
    CHECK(img["credential_hash"]["kind"] == "blob");  // never by value
  }
}

ARCHIVUM_TEST(supervisor_assignments_are_effective_dated) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  REQUIRE(f.post("/api/v1/admin/employees",
                 nlohmann::json{{"employee_number", "S0001"}, {"display_name", "Super"}, {"site_zone", "UTC"}}.dump(), admin_bearer()).status == 201);
  auto a = f.post("/api/v1/admin/supervisors",
                  nlohmann::json{{"employee_number", "E0001"}, {"supervisor_employee_number", "S0001"}, {"effective_from_us", 100}, {"effective_to_us", 200}}.dump(),
                  admin_bearer());
  REQUIRE_MSG(a.status == 201, a.body);
  auto overlap = f.post("/api/v1/admin/supervisors",
                        nlohmann::json{{"employee_number", "E0001"}, {"supervisor_employee_number", "S0001"}, {"effective_from_us", 150}}.dump(),
                        admin_bearer());
  CHECK_MSG(overlap.status == 409, overlap.body);
  auto self = f.post("/api/v1/admin/supervisors",
                     nlohmann::json{{"employee_number", "E0001"}, {"supervisor_employee_number", "E0001"}, {"effective_from_us", 300}}.dump(),
                     admin_bearer());
  CHECK(self.status == 409);
  CHECK(f.post("/api/v1/admin/supervisors",
               nlohmann::json{{"employee_number", "E0001"}, {"supervisor_employee_number", "S0001"}, {"effective_from_us", 200}}.dump(),
               admin_bearer()).status == 201);
}

ARCHIVUM_TEST(break_glass_account_is_console_created_bounded_and_alerted) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  auto& store = f.app->store();
  const std::int64_t now = store.db().now_us();
  auto created = core::create_account(store, f.app->policy(), "breakglass", "break_glass", now);
  REQUIRE_OK(created.status());
  const std::string basic = "Basic " + base64("breakglass:" + created.value().password);
  // Disabled by default.
  CHECK(f.get("/api/v1/whoami", basic).status == 401);
  REQUIRE_OK(core::enable_account(store, f.app->policy(), "breakglass", now + 3600LL * 1'000'000, now));
  auto who = f.get("/api/v1/whoami", basic);
  REQUIRE_MSG(who.status == 200, who.body);
  CHECK(parse(who.body)["kind"] == "account");
  CHECK(parse(who.body)["roles"] == nlohmann::json::array({"admin"}));
  // It can do admin work, audited as the account, and every use is an alert.
  auto grant = f.post("/api/v1/admin/roles", nlohmann::json{{"tid", "t"}, {"oid", "o-bg"}, {"role", "supervisor"}}.dump(), basic);
  REQUIRE_MSG(grant.status == 201, grant.body);
  CHECK(f.log_text().find("\"event\":\"auth.account_used\"") != std::string::npos);
  CHECK(audit_count("account.use") >= 2);
  // Wrong password: 401 and audited.
  CHECK(f.get("/api/v1/whoami", "Basic " + base64("breakglass:nope")).status == 401);
  CHECK(audit_count("auth.account_failed") == 2);  // the disabled attempt above, and this one
  REQUIRE_OK(core::disable_account(store, f.app->policy(), "breakglass", now));
  CHECK(f.get("/api/v1/whoami", basic).status == 401);
}

ARCHIVUM_TEST(health_fails_loudly_until_the_off_host_copy_succeeds) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  // The shipper runs every second; give it a moment.
  int status = 0;
  nlohmann::json body;
  for (int i = 0; i < 50; ++i) {
    auto r = f.get("/healthz");
    status = r.status;
    body = parse(r.body);
    if (status == 200) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK_MSG(status == 200, body.dump());
  CHECK(body["archive"]["ok"] == true);
  CHECK(body["archive"]["backups_shipped"].get<int>() >= 1);
  CHECK(body["status"] == "ok");
  // A destination that stops working: the next pass fails and health says so.
  std::filesystem::rename(f.dir + "/offhost", f.dir + "/offhost-gone");
  Status s = f.app->shipper().ship_once();
  CHECK(!s.ok());
  auto r = f.get("/healthz");
  CHECK_MSG(r.status == 200, "the last success is still within twice the cadence");
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  r = f.get("/healthz");
  CHECK_MSG(r.status == 503, r.body);
  CHECK(parse(r.body)["status"] == "failing");
  CHECK(parse(r.body)["archive"]["problem"].get<std::string>().find("older than twice") != std::string::npos);
  CHECK(f.log_text().find("\"event\":\"archive.ship_failed\"") != std::string::npos);
  std::filesystem::rename(f.dir + "/offhost-gone", f.dir + "/offhost");
  REQUIRE_OK(f.app->shipper().ship_once());
  CHECK(f.get("/healthz").status == 200);
  // The destination holds a backup that opens and checks.
  bool found = false;
  for (const auto& e : std::filesystem::directory_iterator(f.dir + "/offhost")) {
    if (e.path().filename().string().rfind("backup-", 0) == 0) {
      auto vfs = make_os_vfs();
      engine::DbOptions o;
      o.create_if_missing = false;
      auto b = engine::Store::open(*vfs, e.path().string(), o);
      REQUIRE_OK(b.status());
      auto rep = b.value()->check();
      REQUIRE_OK(rep.status());
      CHECK_MSG(rep.value().ok, rep.value().problems[0]);
      found = true;
    }
  }
  CHECK(found);
}

// The contract: every endpoint that takes a body refuses `employee_id`
// with 400 naming the key, at any depth, before doing anything.
ARCHIVUM_TEST(contract_employee_id_in_any_body_is_rejected_on_every_endpoint) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  struct Endpoint {
    std::string path;
    std::string auth;
  };
  const std::vector<Endpoint> endpoints = {
      {"/api/v1/admin/employees", admin_bearer()},
      {"/api/v1/admin/devices", admin_bearer()},
      {"/api/v1/admin/devices/00000000-0000-4000-8000-000000000000/revoke", admin_bearer()},
      {"/api/v1/admin/supervisors", admin_bearer()},
      {"/api/v1/admin/roles", admin_bearer()},
      {"/api/v1/admin/roles/1/revoke", admin_bearer()},
      {"/api/v1/device/heartbeat", "Device 00000000-0000-4000-8000-000000000000:x"},
  };
  const std::vector<std::string> bodies = {
      nlohmann::json{{"employee_id", 1}}.dump(),
      nlohmann::json{{"name", "x"}, {"employee_id", "E0001"}}.dump(),
      nlohmann::json{{"meta", {{"employee_id", 1}}}}.dump(),
      nlohmann::json{{"items", nlohmann::json::array({nlohmann::json{{"employee_id", 1}}})}}.dump(),
  };
  const auto audits_before = audit_count("employee.create") + audit_count("device.enroll");
  for (const Endpoint& e : endpoints) {
    for (const std::string& b : bodies) {
      auto r = f.post(e.path, b, e.auth);
      CHECK_MSG(r.status == 400, e.path << " with " << b << " -> " << r.status << " " << r.body);
      const auto j = parse(r.body);
      CHECK_MSG(j.is_object() && j["message"].get<std::string>().find("employee_id") != std::string::npos, e.path << ": " << r.body);
    }
  }
  CHECK(audit_count("employee.create") + audit_count("device.enroll") == audits_before);
  // Unknown keys are refused too, never ignored.
  auto r = f.post("/api/v1/admin/employees", nlohmann::json{{"employee_number", "X"}, {"display_name", "x"}, {"site_zone", "UTC"}, {"role", "admin"}}.dump(),
                  admin_bearer());
  CHECK_MSG(r.status == 400 && parse(r.body)["message"].get<std::string>().find("role") != std::string::npos, r.body);
}

ARCHIVUM_TEST(zz_shutdown_fixture) {
  auto& f = fixture();
  f.stop();
  CHECK(f.finished.load());
}
