// Local wall-clock arithmetic on the values a punch carries. The server
// has no tz database (GCC 12's libstdc++ has no <chrono> zones); it does
// not need one for Stage 6 because every entry carries its local wall
// clock as the device recorded it, and pay periods, schedules and the
// exception checks are defined on local days and local minutes
// (punchline-updates.md section 8). The instant and the zone are stored
// beside it for a later recomputation under a known tzdb version.
#pragma once

#include <cstdint>
#include <string>

#include "archivum/status.h"

namespace archivum::punchline {

struct LocalTime {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  std::int64_t local_day = 0;  // days since 1970-01-01 in the wall-clock calendar
  int weekday = 0;             // 0 Sunday .. 6 Saturday
  int minute_of_day = 0;
};

// "YYYY-MM-DDTHH:MM:SS" (the wire format), strictly.
Result<LocalTime> parse_local_time(const std::string& text);
// Proleptic Gregorian days since 1970-01-01.
std::int64_t days_from_civil(int year, int month, int day);
int weekday_of_day(std::int64_t local_day);  // 0 Sunday .. 6 Saturday
std::string format_local_day(std::int64_t local_day);  // "YYYY-MM-DD"

}  // namespace archivum::punchline
