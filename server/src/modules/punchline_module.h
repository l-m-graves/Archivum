// The Punchline server module's state: its configuration and the monitor
// thread (device freshness, cutoff escalation). Routes are in
// punchline_routes.cpp; the business logic is in modules/punchline/.
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "archivum/punchline/config.h"
#include "archivum/server/app.h"

namespace archivum::server {

class PunchlineModule : public std::enable_shared_from_this<PunchlineModule> {
 public:
  Status configure(const nlohmann::json& section);
  const punchline::Config& config() const { return config_; }

  void register_routes(App& app);
  void start(App& app);
  void stop();

  // One monitor pass, also callable from tests: opens `device_stale` for
  // devices silent past the threshold and `past_cutoff` for employees
  // behind a period's cutoff; returns how many exceptions it opened.
  Result<int> monitor_once(App& app);

 private:
  void run(App* app);
  punchline::Config config_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool stop_ = false;
};

}  // namespace archivum::server
