// Stage 1 gate, point 4: event-loop behaviour under a deliberately
// saturating client. This test reports; it does not tune. It fails only if
// the server crashes, answers a valid token with anything but 200, or fails
// to answer a probe promptly once the load stops.
//
// Knobs: ARCHIVUM_SAT_SECONDS (3), ARCHIVUM_SAT_CONNECTIONS (64),
// ARCHIVUM_SAT_THREADS (4).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <trantor/net/EventLoopThreadPool.h>

#include "server_fixture.h"
#include "test.h"

using namespace archivum;
using namespace archivum::testing;
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t env_or(const char* name, std::uint64_t fallback) {
  const char* v = std::getenv(name);
  return v ? std::strtoull(v, nullptr, 10) : fallback;
}

struct Stats {
  std::mutex mu;
  std::vector<double> latencies_ms;
  std::atomic<std::uint64_t> ok{0}, non200{0}, errors{0}, timeouts{0};
};

double percentile(std::vector<double>& v, double p) {
  if (v.empty()) return 0;
  const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
  return v[idx];
}

}  // namespace

ARCHIVUM_TEST(saturation_report) {
  ServerFixture f;
  REQUIRE_OK(f.start("1.3", 60));
  const std::string token = "Bearer " + f.issuer->mint(TestIssuer::Claims{});
  const auto seconds = env_or("ARCHIVUM_SAT_SECONDS", 3);
  const auto connections = static_cast<std::size_t>(env_or("ARCHIVUM_SAT_CONNECTIONS", 64));
  const auto threads = static_cast<std::size_t>(env_or("ARCHIVUM_SAT_THREADS", 4));

  trantor::EventLoopThreadPool pool(threads, "archivum-sat");
  pool.start();
  Stats stats;
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> in_flight{0};
  const std::string url = "https://127.0.0.1:" + std::to_string(f.ports.server);

  // Each connection: fire, on response fire again, until stop.
  std::vector<drogon::HttpClientPtr> clients;
  for (std::size_t i = 0; i < connections; ++i) {
    auto client = drogon::HttpClient::newHttpClient(url, pool.getNextLoop(), false, true);
    client->addSSLConfigs({{"VerifyCAFile", f.ca1_cert}});
    clients.push_back(client);
  }
  std::function<void(drogon::HttpClientPtr)> fire;
  fire = [&](drogon::HttpClientPtr client) {
    if (stop.load()) return;
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/v1/whoami");
    req->addHeader("Authorization", token);
    const auto t0 = Clock::now();
    ++in_flight;
    client->sendRequest(
        req,
        [&, client, t0](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
          --in_flight;
          const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
          if (result == drogon::ReqResult::Ok) {
            if (resp->statusCode() == drogon::k200OK) {
              ++stats.ok;
            } else {
              ++stats.non200;
            }
            std::lock_guard<std::mutex> lock(stats.mu);
            stats.latencies_ms.push_back(ms);
          } else if (result == drogon::ReqResult::Timeout) {
            ++stats.timeouts;
          } else {
            ++stats.errors;
          }
          fire(client);
        },
        10.0);
  };
  // Phase 1, connection storm: one request per connection, all at once.
  // Each first request pays connect, client-side TLS context creation, and
  // the handshake. Measured and reported separately from steady state.
  std::atomic<std::uint64_t> warm_done{0};
  const auto warm_start = Clock::now();
  for (auto& c : clients) {
    c->getLoop()->queueInLoop([&, c]() {
      auto req = drogon::HttpRequest::newHttpRequest();
      req->setMethod(drogon::Get);
      req->setPath("/api/v1/whoami");
      req->addHeader("Authorization", token);
      c->sendRequest(req, [&](drogon::ReqResult, const drogon::HttpResponsePtr&) { ++warm_done; }, 30.0);
    });
  }
  while (warm_done.load() < connections && Clock::now() - warm_start < std::chrono::seconds(60)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const double warm_seconds = std::chrono::duration<double>(Clock::now() - warm_start).count();
  REQUIRE_MSG(warm_done.load() == connections, "connection storm did not complete");

  // Phase 2, steady state on warm connections.
  const auto start = Clock::now();
  for (auto& c : clients) c->getLoop()->queueInLoop([&, c]() { fire(c); });

  // Probe /healthz from a separate loop during the load and record how long
  // the event loop takes to answer while saturated.
  std::vector<double> probe_ms;
  while (Clock::now() - start < std::chrono::seconds(seconds)) {
    const auto p0 = Clock::now();
    auto r = f.get("/healthz");
    probe_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - p0).count());
    CHECK_MSG(r.result == drogon::ReqResult::Ok && r.status == 200, "probe failed under load");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop = true;
  // Drain.
  const auto drain_start = Clock::now();
  while (in_flight.load() != 0 && Clock::now() - drain_start < std::chrono::seconds(15)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

  // After load: the server must answer promptly.
  const auto a0 = Clock::now();
  auto after = f.get("/healthz");
  const double after_ms = std::chrono::duration<double, std::milli>(Clock::now() - a0).count();

  std::vector<double> lat;
  {
    std::lock_guard<std::mutex> lock(stats.mu);
    lat = stats.latencies_ms;
  }
  const std::uint64_t total = stats.ok + stats.non200;
  std::printf(
      "  connection storm: %zu connections opened and first-answered in %.2f s (%.1f ms per connection)\n"
      "  saturation: %zu connections on %zu client threads for %.1f s against %u server io threads\n"
      "  responses: %llu ok, %llu non-200, %llu errors, %llu timeouts, %llu still in flight\n"
      "  throughput: %.0f req/s\n"
      "  latency ms: p50 %.2f  p95 %.2f  p99 %.2f  max %.2f\n"
      "  probe /healthz during load ms: max %.2f (n=%zu); after load: %.2f ms\n",
      connections, warm_seconds, warm_seconds * 1000.0 / static_cast<double>(connections),
      connections, threads, elapsed, static_cast<unsigned>(f.config.io_threads),
      static_cast<unsigned long long>(stats.ok.load()), static_cast<unsigned long long>(stats.non200.load()),
      static_cast<unsigned long long>(stats.errors.load()),
      static_cast<unsigned long long>(stats.timeouts.load()),
      static_cast<unsigned long long>(in_flight.load()), static_cast<double>(total) / elapsed,
      percentile(lat, 0.50), percentile(lat, 0.95), percentile(lat, 0.99),
      lat.empty() ? 0.0 : *std::max_element(lat.begin(), lat.end()),
      probe_ms.empty() ? 0.0 : *std::max_element(probe_ms.begin(), probe_ms.end()), probe_ms.size(),
      after_ms);

  CHECK(stats.non200.load() == 0);
  CHECK(total > 0);
  CHECK_MSG(after.result == drogon::ReqResult::Ok && after.status == 200, "server unresponsive after load");
  CHECK_MSG(after_ms < 2000.0, "server slow to recover after load");
  CHECK_MSG(in_flight.load() == 0, "requests never completed after load stopped");
  clients.clear();
  f.stop();
}
