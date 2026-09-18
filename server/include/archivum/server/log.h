// Structured logging: one JSON object per line, to stderr or a file.
// Fields are named, never formatted into a message, so a log line is
// queryable. Request bodies are never logged (docs/confidentiality-check.md,
// "HTTP request logging"); identifiers, outcomes and timings are.
//
// Levels: info, warn, error, and alert. Alert is for events a person must
// see: a break-glass use, an unknown principal, a failing archive copy.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "archivum/status.h"

namespace archivum::server {

enum class LogLevel { Info, Warn, Error, Alert };
const char* to_string(LogLevel level);

class Logger {
 public:
  // Empty `file`: stderr. `min_level` filters info and warn; error and
  // alert are always written.
  static Result<std::shared_ptr<Logger>> open(const std::string& file, const std::string& min_level);
  ~Logger();

  void log(LogLevel level, std::string_view event, nlohmann::json fields = nlohmann::json::object());
  std::uint64_t lines_written() const;

 private:
  Logger(FILE* out, bool owns, LogLevel min);
  FILE* out_;
  bool owns_;
  LogLevel min_;
  mutable std::mutex mu_;
  std::uint64_t lines_ = 0;
};

// Process-wide logger for request handlers; set by the App.
void set_logger(std::shared_ptr<Logger> logger);
std::shared_ptr<Logger> logger();
void log(LogLevel level, std::string_view event, nlohmann::json fields = nlohmann::json::object());

// A fresh request id: 16 random bytes, hex.
std::string new_request_id();

}  // namespace archivum::server
