#include "archivum/punchline/tzif.h"

#include <algorithm>
#include <cstring>

namespace archivum::punchline::tz {
namespace {

std::uint32_t be32(const std::byte* p) {
  return (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[0])) << 24) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[1])) << 16) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned>(p[2])) << 8) |
         static_cast<std::uint32_t>(std::to_integer<unsigned>(p[3]));
}
std::int64_t be64(const std::byte* p) {
  return static_cast<std::int64_t>((static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4));
}

struct Header {
  int version = 0;
  std::uint32_t isutcnt = 0, isstdcnt = 0, leapcnt = 0, timecnt = 0, typecnt = 0, charcnt = 0;
};

Result<Header> read_header(std::span<const std::byte> d, std::size_t at) {
  if (d.size() < at + 44) return Status::corrupt("TZif: truncated header");
  const std::byte* p = d.data() + at;
  if (std::memcmp(p, "TZif", 4) != 0) return Status::corrupt("TZif: bad magic");
  Header h;
  const char v = static_cast<char>(p[4]);
  if (v == '\0') h.version = 1;
  else if (v >= '2' && v <= '9') h.version = v - '0';
  else return Status::corrupt("TZif: unknown version byte");
  h.isutcnt = be32(p + 20);
  h.isstdcnt = be32(p + 24);
  h.leapcnt = be32(p + 28);
  h.timecnt = be32(p + 32);
  h.typecnt = be32(p + 36);
  h.charcnt = be32(p + 40);
  if (h.typecnt == 0 || h.typecnt > 256 || h.timecnt > 1u << 20 || h.charcnt > 1u << 16 || h.leapcnt > 1u << 16) {
    return Status::corrupt("TZif: implausible counts");
  }
  if (h.isstdcnt != 0 && h.isstdcnt != h.typecnt) return Status::corrupt("TZif: isstdcnt");
  if (h.isutcnt != 0 && h.isutcnt != h.typecnt) return Status::corrupt("TZif: isutcnt");
  return h;
}

std::size_t block_size(const Header& h, int time_size) {
  return h.timecnt * static_cast<std::size_t>(time_size) + h.timecnt + h.typecnt * 6 + h.charcnt +
         h.leapcnt * (static_cast<std::size_t>(time_size) + 4) + h.isstdcnt + h.isutcnt;
}

// Days from civil, proleptic Gregorian (Howard Hinnant).
std::int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = static_cast<std::int64_t>(y) - era * 400;
  const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
int weekday_of(std::int64_t days) {
  const std::int64_t w = (days + 4) % 7;
  return static_cast<int>(w < 0 ? w + 7 : w);
}
bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int year_of_instant(std::int64_t instant) {
  std::int64_t z = (instant >= 0 ? instant : instant - 86399) / 86400 + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t m = mp + (mp < 10 ? 3 : -9);
  return static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

// --- POSIX TZ string ------------------------------------------------------

struct Cursor {
  const std::string& s;
  std::size_t i = 0;
  bool done() const { return i >= s.size(); }
  char peek() const { return done() ? '\0' : s[i]; }
};

Result<std::string> parse_name(Cursor& c) {
  std::string name;
  if (c.peek() == '<') {
    ++c.i;
    while (!c.done() && c.peek() != '>') name += c.s[c.i++];
    if (c.peek() != '>') return Status::corrupt("TZ string: unterminated <name>");
    ++c.i;
  } else {
    while (!c.done() && ((c.peek() >= 'A' && c.peek() <= 'Z') || (c.peek() >= 'a' && c.peek() <= 'z'))) name += c.s[c.i++];
  }
  if (name.size() < 3) return Status::corrupt("TZ string: name shorter than three characters");
  return name;
}

// [+-]hh[:mm[:ss]]; `allow_sign_only` for rule times that may exceed 24 h.
Result<std::int64_t> parse_hms(Cursor& c, int max_hours) {
  int sign = 1;
  if (c.peek() == '+') ++c.i;
  else if (c.peek() == '-') {
    sign = -1;
    ++c.i;
  }
  auto number = [&](int& out) {
    if (c.peek() < '0' || c.peek() > '9') return false;
    out = 0;
    while (c.peek() >= '0' && c.peek() <= '9') out = out * 10 + (c.s[c.i++] - '0');
    return true;
  };
  int h = 0, m = 0, s = 0;
  if (!number(h)) return Status::corrupt("TZ string: expected hours");
  if (c.peek() == ':') {
    ++c.i;
    if (!number(m)) return Status::corrupt("TZ string: expected minutes");
    if (c.peek() == ':') {
      ++c.i;
      if (!number(s)) return Status::corrupt("TZ string: expected seconds");
    }
  }
  if (h > max_hours || m > 59 || s > 59) return Status::corrupt("TZ string: time out of range");
  return sign * (static_cast<std::int64_t>(h) * 3600 + m * 60 + s);
}

Result<RuleDate> parse_rule_date(Cursor& c) {
  RuleDate d;
  auto number = [&](int& out) {
    if (c.peek() < '0' || c.peek() > '9') return false;
    out = 0;
    while (c.peek() >= '0' && c.peek() <= '9') out = out * 10 + (c.s[c.i++] - '0');
    return true;
  };
  if (c.peek() == 'M') {
    ++c.i;
    d.kind = RuleDate::Kind::MonthWeekDay;
    if (!number(d.month) || c.peek() != '.') return Status::corrupt("TZ string: Mm.w.d");
    ++c.i;
    if (!number(d.week) || c.peek() != '.') return Status::corrupt("TZ string: Mm.w.d");
    ++c.i;
    if (!number(d.weekday)) return Status::corrupt("TZ string: Mm.w.d");
    if (d.month < 1 || d.month > 12 || d.week < 1 || d.week > 5 || d.weekday < 0 || d.weekday > 6) {
      return Status::corrupt("TZ string: Mm.w.d out of range");
    }
  } else if (c.peek() == 'J') {
    ++c.i;
    d.kind = RuleDate::Kind::JulianNoLeap;
    if (!number(d.day) || d.day < 1 || d.day > 365) return Status::corrupt("TZ string: Jn");
  } else {
    d.kind = RuleDate::Kind::JulianZeroBased;
    if (!number(d.day) || d.day < 0 || d.day > 365) return Status::corrupt("TZ string: n");
  }
  if (c.peek() == '/') {
    ++c.i;
    auto t = parse_hms(c, 167);  // RFC 8536 v3: -167 to 167 hours
    if (!t.ok()) return t.status();
    d.time = t.value();
  }
  return d;
}

}  // namespace

Result<PosixRule> Zone::parse_posix(const std::string& tz) {
  Cursor c{tz};
  PosixRule r;
  auto std_name = parse_name(c);
  if (!std_name.ok()) return std_name.status();
  r.std.abbrev = std_name.value();
  auto std_off = parse_hms(c, 24);
  if (!std_off.ok()) return std_off.status();
  r.std.utoff = static_cast<std::int32_t>(-std_off.value());  // POSIX offsets are west-positive
  r.std.is_dst = false;
  if (c.done()) return r;
  auto dst_name = parse_name(c);
  if (!dst_name.ok()) return dst_name.status();
  LocalTimeType dst;
  dst.abbrev = dst_name.value();
  dst.is_dst = true;
  dst.utoff = r.std.utoff + 3600;
  if (!c.done() && c.peek() != ',') {
    auto off = parse_hms(c, 24);
    if (!off.ok()) return off.status();
    dst.utoff = static_cast<std::int32_t>(-off.value());
  }
  if (c.peek() != ',') return Status::corrupt("TZ string: dst without rules");
  ++c.i;
  auto start = parse_rule_date(c);
  if (!start.ok()) return start.status();
  if (c.peek() != ',') return Status::corrupt("TZ string: missing end rule");
  ++c.i;
  auto end = parse_rule_date(c);
  if (!end.ok()) return end.status();
  if (!c.done()) return Status::corrupt("TZ string: trailing characters");
  r.dst = dst;
  r.start = start.value();
  r.end = end.value();
  return r;
}

Result<Zone> Zone::parse(std::span<const std::byte> data) {
  auto h1 = read_header(data, 0);
  if (!h1.ok()) return h1.status();
  Zone z;
  z.version_ = h1.value().version;
  std::size_t at = 44;
  Header h = h1.value();
  int time_size = 4;
  if (h.version >= 2) {
    at += block_size(h, 4);
    auto h2 = read_header(data, at);
    if (!h2.ok()) return h2.status();
    h = h2.value();
    at += 44;
    time_size = 8;
  }
  if (data.size() < at + block_size(h, time_size)) return Status::corrupt("TZif: truncated data block");
  const std::byte* p = data.data() + at;
  z.transition_times_.reserve(h.timecnt);
  for (std::uint32_t i = 0; i < h.timecnt; ++i) {
    const std::int64_t t = time_size == 8 ? be64(p + i * 8) : static_cast<std::int32_t>(be32(p + i * 4));
    if (i > 0 && t <= z.transition_times_.back()) return Status::corrupt("TZif: transitions not increasing");
    z.transition_times_.push_back(t);
  }
  p += h.timecnt * static_cast<std::size_t>(time_size);
  for (std::uint32_t i = 0; i < h.timecnt; ++i) {
    const std::uint8_t idx = std::to_integer<std::uint8_t>(p[i]);
    if (idx >= h.typecnt) return Status::corrupt("TZif: transition type out of range");
    z.transition_types_.push_back(idx);
  }
  p += h.timecnt;
  const std::byte* chars = p + h.typecnt * 6;
  for (std::uint32_t i = 0; i < h.typecnt; ++i) {
    LocalTimeType t;
    t.utoff = static_cast<std::int32_t>(be32(p + i * 6));
    t.is_dst = std::to_integer<unsigned>(p[i * 6 + 4]) != 0;
    const unsigned idx = std::to_integer<unsigned>(p[i * 6 + 5]);
    if (idx >= h.charcnt) return Status::corrupt("TZif: abbreviation index out of range");
    for (unsigned k = idx; k < h.charcnt && chars[k] != std::byte{0}; ++k) t.abbrev += static_cast<char>(chars[k]);
    z.types_.push_back(std::move(t));
  }
  at += block_size(h, time_size);
  if (h.version >= 2) {
    // Footer: "\n" TZ "\n".
    if (data.size() < at + 2 || data[at] != std::byte{'\n'}) return Status::corrupt("TZif: missing footer");
    std::string tz;
    std::size_t k = at + 1;
    while (k < data.size() && data[k] != std::byte{'\n'}) tz += static_cast<char>(data[k++]);
    if (k >= data.size()) return Status::corrupt("TZif: unterminated footer");
    if (!tz.empty()) {
      auto rule = parse_posix(tz);
      if (!rule.ok()) return rule.status();
      z.footer_ = rule.value();
    }
  }
  return z;
}

Zone::Local Zone::type_local(std::size_t type_index) const {
  const LocalTimeType& t = types_[type_index];
  return Local{t.utoff, t.is_dst, t.abbrev};
}

// The instant at which rule date `d` of `year` takes effect, given the
// offset in force before it (the rule's time is local time under that offset).
std::int64_t Zone::rule_transition(int year, const RuleDate& d, std::int64_t offset_before) const {
  std::int64_t day = 0;
  switch (d.kind) {
    case RuleDate::Kind::MonthWeekDay: {
      const std::int64_t first = days_from_civil(year, d.month, 1);
      const int first_wd = weekday_of(first);
      int delta = d.weekday - first_wd;
      if (delta < 0) delta += 7;
      day = first + delta + static_cast<std::int64_t>(d.week - 1) * 7;
      static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      const std::int64_t last = first + dim[d.month - 1] + (d.month == 2 && leap(year) ? 1 : 0) - 1;
      while (day > last) day -= 7;  // week 5 means the last such weekday
      break;
    }
    case RuleDate::Kind::JulianNoLeap: {
      int n = d.day;  // 1..365, Feb 29 never counted
      if (leap(year) && n >= 60) ++n;
      day = days_from_civil(year, 1, 1) + n - 1;
      break;
    }
    case RuleDate::Kind::JulianZeroBased:
      day = days_from_civil(year, 1, 1) + d.day;
      break;
  }
  return day * 86400 + d.time - offset_before;
}

Zone::Local Zone::rule_local(std::int64_t instant) const {
  const PosixRule& r = *footer_;
  if (!r.dst) return Local{r.std.utoff, false, r.std.abbrev};
  // The rule's transitions for the surrounding years, then the latest one
  // at or before the instant decides. Three years cover a transition near
  // new year in either hemisphere.
  const int year = year_of_instant(instant + r.std.utoff);
  std::vector<std::pair<std::int64_t, bool>> ts;  // (instant, dst after)
  for (int y : {year - 1, year, year + 1}) {
    ts.emplace_back(rule_transition(y, r.start, r.std.utoff), true);
    ts.emplace_back(rule_transition(y, r.end, r.dst->utoff), false);
  }
  std::sort(ts.begin(), ts.end());
  bool dst = false;
  for (const auto& [t, d] : ts) {
    if (t <= instant) dst = d;
  }
  return dst ? Local{r.dst->utoff, true, r.dst->abbrev} : Local{r.std.utoff, false, r.std.abbrev};
}

Zone::Local Zone::local_of(std::int64_t instant) const {
  if (transition_times_.empty()) {
    if (footer_) return rule_local(instant);
    return type_local(0);
  }
  if (instant < transition_times_.front()) {
    // Before the first transition: the first non-DST type (RFC 8536 §3.2), else type 0.
    for (std::size_t i = 0; i < types_.size(); ++i) {
      if (!types_[i].is_dst) return type_local(i);
    }
    return type_local(0);
  }
  auto it = std::upper_bound(transition_times_.begin(), transition_times_.end(), instant);
  const std::size_t idx = static_cast<std::size_t>(it - transition_times_.begin()) - 1;
  if (idx + 1 == transition_times_.size() && footer_) {
    // At or after the last transition: the footer rule governs, and by
    // construction agrees with the last transition's type at that instant.
    return rule_local(instant);
  }
  return type_local(transition_types_[idx]);
}

std::vector<std::int64_t> Zone::instants_of(std::int64_t local_seconds) const {
  std::vector<std::int64_t> candidates;
  auto consider = [&](std::int64_t offset) {
    const std::int64_t t = local_seconds - offset;
    if (local_of(t).offset == offset) candidates.push_back(t);
  };
  consider(local_of(local_seconds - 36 * 3600).offset);
  consider(local_of(local_seconds + 36 * 3600).offset);
  for (const LocalTimeType& t : types_) consider(t.utoff);
  if (footer_) {
    consider(footer_->std.utoff);
    if (footer_->dst) consider(footer_->dst->utoff);
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
  return candidates;
}

std::vector<Zone::Transition> Zone::transitions(std::int64_t from, std::int64_t to) const {
  std::vector<Transition> out;
  Local prev = local_of(from);
  // Explicit transitions in range.
  for (std::size_t i = 0; i < transition_times_.size(); ++i) {
    const std::int64_t t = transition_times_[i];
    if (t < from || t >= to) continue;
    Local after = local_of(t);
    if (after.offset != prev.offset || after.is_dst != prev.is_dst || after.abbrev != prev.abbrev) {
      out.push_back({t, after});
    }
    prev = after;
  }
  // Rule transitions past the last explicit one.
  if (footer_ && footer_->dst) {
    const std::int64_t rule_from = transition_times_.empty() ? from : std::max(from, transition_times_.back());
    const int y0 = year_of_instant(rule_from) - 1, y1 = year_of_instant(to) + 1;
    std::vector<std::int64_t> times;
    for (int y = y0; y <= y1; ++y) {
      times.push_back(rule_transition(y, footer_->start, footer_->std.utoff));
      times.push_back(rule_transition(y, footer_->end, footer_->dst->utoff));
    }
    std::sort(times.begin(), times.end());
    Local p = local_of(rule_from);
    for (std::int64_t t : times) {
      if (t < rule_from || t >= to) continue;
      if (t <= (out.empty() ? from - 1 : out.back().at)) continue;
      Local after = local_of(t);
      if (after.offset != p.offset || after.is_dst != p.is_dst) out.push_back({t, after});
      p = after;
    }
  }
  std::sort(out.begin(), out.end(), [](const Transition& a, const Transition& b) { return a.at < b.at; });
  return out;
}

Result<Zone> zone(const std::string& name) {
  const EmbeddedDatabase& db = embedded_database();
  if (db.count == 0) return Status::unsupported("no time zone database is embedded in this binary (tools/tzdata/generate.py)");
  for (std::size_t i = 0; i < db.count; ++i) {
    if (name == db.zones[i].name) {
      return Zone::parse(std::span<const std::byte>(reinterpret_cast<const std::byte*>(db.zones[i].data), db.zones[i].size));
    }
  }
  return Status::not_found("unknown time zone: " + name);
}

Result<std::string> database_version() {
  const EmbeddedDatabase& db = embedded_database();
  if (db.count == 0) return Status::unsupported("no time zone database is embedded in this binary");
  return std::string(db.version);
}

}  // namespace archivum::punchline::tz
