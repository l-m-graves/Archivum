// The end-to-end server for the rewritten Punchline client
// (tests/e2e/client_e2e.sh): the server test fixture (its own PKI and
// issuer, a store with an admin principal and the employee E0001 in
// America/Los_Angeles), plus an open pay period covering today and a
// device enrolled for the employee. It prints one JSON object with what
// the client and the script need (URL, CA file, the device credential, an
// admin bearer, the log file) and then runs until it is killed.
#include <chrono>
#include <cstdio>
#include <thread>

#include <nlohmann/json.hpp>

#include "archivum/punchline/localtime.h"
#include "server_fixture.h"

using namespace archivum;
using namespace archivum::testing;

int main() {
  ServerFixture f;
  f.punchline_config = nlohmann::json{{"long_shift_hours", 24}};
  if (Status s = f.start("1.3", 1); !s.ok()) {
    std::fprintf(stderr, "e2e server: start failed: %s\n", s.to_string().c_str());
    return 1;
  }
  if (!f.wait_healthy()) {
    std::fprintf(stderr, "e2e server: never healthy\n");
    return 1;
  }
  TestIssuer::Claims c;
  c.oid = ServerFixture::kAdminOid;
  const std::string admin = "Bearer " + f.issuer->mint(c);

  // A period from 30 days ago to 30 days ahead, so today's punches land in it.
  const std::int64_t today = f.app->store().db().now_us() / 1'000'000 / 86400;
  auto period = f.post("/api/v1/admin/periods",
                       nlohmann::json{{"start_day", punchline::format_local_day(today - 30)},
                                      {"end_day", punchline::format_local_day(today + 30)},
                                      {"site_zone", "America/Los_Angeles"},
                                      {"tzdb_version", "placeholder"}}
                           .dump(),
                       admin);
  if (period.status != 201) {
    std::fprintf(stderr, "e2e server: period not created: %d %s\n", period.status, period.body.c_str());
    return 1;
  }
  auto dev = f.post("/api/v1/admin/devices", nlohmann::json{{"employee_number", "E0001"}, {"name", "e2e device"}}.dump(), admin);
  if (dev.status != 201) {
    std::fprintf(stderr, "e2e server: device not enrolled: %d %s\n", dev.status, dev.body.c_str());
    return 1;
  }
  const nlohmann::json d = nlohmann::json::parse(dev.body);
  nlohmann::json out;
  out["ready"] = true;
  out["url"] = "https://127.0.0.1:" + std::to_string(f.ports.server);
  out["ca_file"] = f.ca1_cert;
  out["credential"] = d["credential"];
  out["device_uuid"] = d["device_uuid"];
  out["admin_bearer"] = admin;
  out["employee_number"] = "E0001";
  out["period_id"] = nlohmann::json::parse(period.body)["id"];
  out["log_file"] = f.dir + "/server.log";
  std::printf("%s\n", out.dump().c_str());
  std::fflush(stdout);
  while (!f.finished) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return 0;
}
