#include "archivum/server/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace archivum::server {
namespace {

std::shared_ptr<Logger>& global() {
  static std::shared_ptr<Logger> g;
  return g;
}
std::mutex& global_mu() {
  static std::mutex m;
  return m;
}

std::string iso_now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() % 1000000;
  struct tm tm {};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(us));
  return buf;
}

}  // namespace

const char* to_string(LogLevel level) {
  switch (level) {
    case LogLevel::Info:
      return "info";
    case LogLevel::Warn:
      return "warn";
    case LogLevel::Error:
      return "error";
    case LogLevel::Alert:
      return "alert";
  }
  return "info";
}

Result<std::shared_ptr<Logger>> Logger::open(const std::string& file, const std::string& min_level) {
  LogLevel min = LogLevel::Info;
  if (min_level == "warn") min = LogLevel::Warn;
  else if (min_level == "error") min = LogLevel::Error;
  else if (min_level != "info") return Status::invalid_argument("log level: " + min_level);
  if (file.empty()) return std::shared_ptr<Logger>(new Logger(stderr, false, min));
  FILE* f = std::fopen(file.c_str(), "ab");
  if (f == nullptr) return Status::io("cannot open log file: " + file);
  return std::shared_ptr<Logger>(new Logger(f, true, min));
}

Logger::Logger(FILE* out, bool owns, LogLevel min) : out_(out), owns_(owns), min_(min) {}
Logger::~Logger() {
  if (owns_ && out_ != nullptr) std::fclose(out_);
}

void Logger::log(LogLevel level, std::string_view event, nlohmann::json fields) {
  if (static_cast<int>(level) < static_cast<int>(min_) && level != LogLevel::Alert) return;
  nlohmann::json line;
  line["ts"] = iso_now();
  line["level"] = to_string(level);
  line["event"] = std::string(event);
  if (fields.is_object()) {
    for (auto& [k, v] : fields.items()) line[k] = v;
  }
  const std::string text = line.dump() + "\n";
  std::lock_guard<std::mutex> lock(mu_);
  std::fputs(text.c_str(), out_);
  std::fflush(out_);
  ++lines_;
}

std::uint64_t Logger::lines_written() const {
  std::lock_guard<std::mutex> lock(mu_);
  return lines_;
}

void set_logger(std::shared_ptr<Logger> l) {
  std::lock_guard<std::mutex> lock(global_mu());
  global() = std::move(l);
}

std::shared_ptr<Logger> logger() {
  std::lock_guard<std::mutex> lock(global_mu());
  return global();
}

void log(LogLevel level, std::string_view event, nlohmann::json fields) {
  auto l = logger();
  if (l) l->log(level, event, std::move(fields));
}

std::string new_request_id() {
  std::random_device rd;
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (int i = 0; i < 4; ++i) {
    const std::uint32_t v = rd();
    for (int k = 0; k < 8; ++k) s += hex[(v >> (4 * k)) & 15];
  }
  return s;
}

}  // namespace archivum::server
