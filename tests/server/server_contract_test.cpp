// The batch-ingest contract shared with the FastAPI prototype
// (tests/contract/scenarios.json), run against Archivum. The Python runner
// in tests/contract/ runs the same file against the prototype; the two
// together are the "contract suite identical between both servers" gate
// of punchline-updates.md section 10. Each scenario maps the abstract
// entry to Archivum's wire shape (punches under a device credential) and
// asserts the outcomes the file names.
#include <vector>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "archivum/core/schema.h"
#include "server_fixture.h"
#include "test.h"

using namespace archivum;
using namespace archivum::testing;

namespace {

constexpr std::int64_t kUs = 1'000'000;
constexpr std::int64_t kMonday = 1'709'510'400LL * kUs;  // 2024-03-04T00:00:00Z

ServerFixture& fixture() {
  static ServerFixture* f = nullptr;
  if (f == nullptr) {
    f = new ServerFixture();
    f->punchline_config = nlohmann::json{{"long_shift_hours", 24}};
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

nlohmann::json parse(const std::string& body) { return nlohmann::json::parse(body, nullptr, false); }

std::string uid(int n) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012d", n);
  return buf;
}

nlohmann::json load_scenarios() {
  const char* src = std::getenv("ARCHIVUM_SOURCE_DIR");
  std::string path = std::string(src ? src : ".") + "/tests/contract/scenarios.json";
  std::ifstream in(path);
  if (!in) return nlohmann::json();
  std::stringstream ss;
  ss << in.rdbuf();
  return parse(ss.str());
}

// Archivum's wire shape for one abstract entry: an in punch and an out
// punch `minutes` apart. Entry n takes uuids 2n and 2n+1 and the sequence
// numbers likewise; the "id" the scenario names is reported back as the
// in punch's uuid.
std::vector<nlohmann::json> wire_entry(const nlohmann::json& abstract, int& seq, int scenario_no) {
  const int id = abstract["id"].get<int>();
  const int base = scenario_no * 1000;  // uuids unique per scenario (the file assumes a fresh store)
  const int minutes = abstract.value("minutes", 480);
  const std::int64_t in_at = kMonday + 9 * 3600 * kUs + id * 86400 * kUs;  // one day per entry, so pairing never crosses
  const std::string invalid = abstract.value("invalid", "");
  auto punch = [&](const std::string& uuid, const char* kind, std::int64_t at, int minute_of_day) {
    nlohmann::json e;
    e["entry_uuid"] = uuid;
    e["journal_sequence"] = seq++;
    e["kind"] = kind;
    e["device_time_us"] = at;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "2024-03-%02dT%02d:%02d:00", 4 + id, (minute_of_day / 60) % 24, minute_of_day % 60);
    e["local_time"] = buf;
    e["site_zone"] = "UTC";
    e["tzdb_version"] = "2024a";
    if (abstract.value("assert_employee_id", false)) e["employee_id"] = "E0001";
    return e;
  };
  nlohmann::json in = punch(uid(base + 2 * id), "in", in_at, 9 * 60);
  nlohmann::json out = punch(uid(base + 2 * id + 1), "out", in_at + minutes * 60 * kUs, 9 * 60 + minutes);
  // An invalid abstract entry is invalid in both of its punches, so the
  // two servers' stored counts stay comparable.
  if (invalid == "missing_required_field") {
    in.erase("kind");
    out.erase("kind");
  }
  if (invalid == "uuid") {
    in["entry_uuid"] = "not-a-uuid";
    out["entry_uuid"] = "not-a-uuid";
  }
  return {in, out};
}

std::size_t stored() {
  auto rd = fixture().app->store().begin_read();
  std::size_t n = 0;
  (void)rd.value()->scan("time_entries", "", std::nullopt, std::nullopt, false, [&](const engine::Row&) {
    ++n;
    return true;
  });
  return n;
}

}  // namespace

ARCHIVUM_TEST(contract_scenarios_hold_on_archivum) {
  auto& f = fixture();
  REQUIRE_OK(f.run_status);
  REQUIRE(f.wait_healthy());
  const nlohmann::json doc = load_scenarios();
  REQUIRE_MSG(doc.is_object() && doc.contains("scenarios"), "set ARCHIVUM_SOURCE_DIR to the repository root");
  // A period covering every day the scenarios use, and the device.
  REQUIRE(f.post("/api/v1/admin/periods",
                 nlohmann::json{{"start_day", "2024-03-04"}, {"end_day", "2024-03-31"}, {"site_zone", "UTC"}, {"tzdb_version", "2024a"}}.dump(),
                 admin_bearer()).status == 201);
  int scenario_no = 0;
  for (const nlohmann::json& scenario : doc["scenarios"]) {
    ++scenario_no;
    const std::string name = scenario["name"].get<std::string>();
    // A fresh device per scenario: the fresh-store assumption of the file,
    // approximated by a fresh journal id, fresh uuids and a fresh device.
    auto dev = f.post("/api/v1/admin/devices", nlohmann::json{{"employee_number", "E0001"}, {"name", "contract-" + name}}.dump(), admin_bearer());
    REQUIRE_MSG(dev.status == 201, dev.body);
    const std::string credential = "Device " + parse(dev.body)["credential"].get<std::string>();
    const std::size_t before = stored();
    int seq = 1;
    int step_no = 0;
    for (const nlohmann::json& step : scenario["steps"]) {
      ++step_no;
      nlohmann::json body;
      body["batch_uuid"] = uid(100000 * scenario_no + step_no);
      body["journal_id"] = uid(900000 + scenario_no);
      body["entries"] = nlohmann::json::array();
      std::map<int, std::string> in_uuid_of;
      for (const nlohmann::json& abstract : step["entries"]) {
        int local_seq = seq;
        for (nlohmann::json& e : wire_entry(abstract, local_seq, scenario_no)) body["entries"].push_back(e);
        // The same abstract id twice in one batch is the same punches twice (same sequences): a client-side duplicate.
        if (!in_uuid_of.count(abstract["id"].get<int>())) seq = local_seq;
        in_uuid_of[abstract["id"].get<int>()] = uid(scenario_no * 1000 + 2 * abstract["id"].get<int>());
      }
      const std::string auth = step.value("auth", "") == "none" ? "" : credential;
      auto r = f.post("/api/v1/device/sync", body.dump(), auth);
      nlohmann::json expect = step.value("expect", nlohmann::json::object());
      if (step.contains("expect_by_server")) {
        for (const auto& [k, v] : step["expect_by_server"]["archivum"].items()) expect[k] = v;
      }
      const std::string where = name + " step " + std::to_string(step_no) + ": " + r.body;
      if (expect.contains("http_status")) {
        CHECK_MSG(r.status == expect["http_status"].get<int>(), where);
      } else {
        REQUIRE_MSG(r.status == 200, where);
        const nlohmann::json j = parse(r.body);
        // Archivum answers per punch; the abstract id is accepted when its in punch is.
        auto accepted_ids = [&]() {
          std::vector<int> out;
          for (const auto& u : j["accepted"]) {
            for (const auto& [id, in_uuid] : in_uuid_of) {
              if (u.get<std::string>() == in_uuid) out.push_back(id);
            }
          }
          return out;
        }();
        if (expect.contains("accepted")) {
          std::vector<int> want;
          for (const auto& n : expect["accepted"]) want.push_back(n.get<int>());
          CHECK_MSG(accepted_ids == want, where);
        }
        if (expect.contains("accepted_any_of")) {
          bool any = false;
          for (const auto& option : expect["accepted_any_of"]) {
            std::vector<int> want;
            for (const auto& n : option) want.push_back(n.get<int>());
            any = any || accepted_ids == want;
          }
          CHECK_MSG(any, where);
        }
        if (expect.contains("rejected")) {
          std::vector<int> got;
          for (const auto& x : j["rejected"]) {
            for (const auto& [id, in_uuid] : in_uuid_of) {
              if (x["uuid"].get<std::string>() == in_uuid) got.push_back(id);
            }
          }
          std::vector<int> want;
          for (const auto& n : expect["rejected"]) want.push_back(n.get<int>());
          CHECK_MSG(got == want, where);
        }
        if (expect.contains("rejected_count")) {
          // Rejected abstract entries: distinct uuids as sent (a malformed one is echoed as sent).
          std::set<std::string> distinct;
          for (const auto& x : j["rejected"]) {
            const std::string u = x["uuid"].get<std::string>();
            bool is_out = false;
            for (const auto& [id, in_uuid] : in_uuid_of) is_out = is_out || u == uid(scenario_no * 1000 + 2 * id + 1);
            if (!is_out) distinct.insert(u);
          }
          CHECK_MSG(distinct.size() == expect["rejected_count"].get<std::size_t>(), where);
        }
        if (expect.contains("rejected_reason_mentions")) {
          const std::string reason = j["rejected"][0]["reason"].get<std::string>();
          bool any = false;
          for (const auto& word : expect["rejected_reason_mentions"]) any = any || reason.find(word.get<std::string>()) != std::string::npos;
          CHECK_MSG(any, where);
        }
        if (expect.value("flagged", false)) {
          // Archivum accepts the long shift and opens a `long_shift` item for the supervisor.
          CHECK_MSG(j["exceptions_opened"].get<int>() >= 1, where);
        }
      }
      // "stored" counts abstract entries; Archivum stores two punches per entry.
      if (expect.contains("stored")) CHECK_MSG(stored() - before == 2 * expect["stored"].get<std::size_t>(), where << " stored " << stored() - before);
      if (expect.contains("stored_any_of")) {
        bool any = false;
        for (const auto& n : expect["stored_any_of"]) any = any || stored() - before == 2 * n.get<std::size_t>();
        CHECK_MSG(any, where);
      }
    }
    // Every device the contract used gets revoked so the next scenario is independent of it.
    REQUIRE(f.post("/api/v1/admin/devices/" + parse(dev.body)["device_uuid"].get<std::string>() + "/revoke",
                   nlohmann::json{{"reason", "scenario done"}}.dump(), admin_bearer()).status == 200);
  }
  CHECK(scenario_no == 8);
}

ARCHIVUM_TEST(zz_shutdown_fixture) {
  auto& f = fixture();
  f.stop();
  CHECK(f.finished.load());
}
