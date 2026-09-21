#include "archivum/punchline/localtime.h"

#include <cstdio>

namespace archivum::punchline {

// Howard Hinnant's days_from_civil, valid for the proleptic Gregorian
// calendar over the int range.
std::int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = static_cast<std::int64_t>(y) - era * 400;
  const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

int weekday_of_day(std::int64_t local_day) {
  // 1970-01-01 was a Thursday (4).
  const std::int64_t w = (local_day + 4) % 7;
  return static_cast<int>(w < 0 ? w + 7 : w);
}

std::string format_local_day(std::int64_t z) {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp + (mp < 10 ? 3 : -9);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", static_cast<int>(y + (m <= 2)), static_cast<int>(m), static_cast<int>(d));
  return buf;
}

Result<LocalTime> parse_local_time(const std::string& s) {
  if (s.size() != 19 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
    return Status::invalid_argument("local_time must be YYYY-MM-DDTHH:MM:SS");
  }
  auto num = [&](std::size_t at, std::size_t len, int& out) {
    out = 0;
    for (std::size_t i = at; i < at + len; ++i) {
      if (s[i] < '0' || s[i] > '9') return false;
      out = out * 10 + (s[i] - '0');
    }
    return true;
  };
  LocalTime t;
  if (!num(0, 4, t.year) || !num(5, 2, t.month) || !num(8, 2, t.day) || !num(11, 2, t.hour) || !num(14, 2, t.minute) ||
      !num(17, 2, t.second)) {
    return Status::invalid_argument("local_time must be YYYY-MM-DDTHH:MM:SS");
  }
  static const int days_in_month[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (t.year % 4 == 0 && t.year % 100 != 0) || t.year % 400 == 0;
  if (t.year < 1970 || t.year > 9999 || t.month < 1 || t.month > 12 || t.day < 1 ||
      t.day > days_in_month[t.month - 1] - (t.month == 2 && !leap ? 1 : 0) || t.hour > 23 || t.minute > 59 || t.second > 60) {
    return Status::invalid_argument("local_time is not a valid wall-clock time");
  }
  t.local_day = days_from_civil(t.year, t.month, t.day);
  t.weekday = weekday_of_day(t.local_day);
  t.minute_of_day = t.hour * 60 + t.minute;
  return t;
}

}  // namespace archivum::punchline
