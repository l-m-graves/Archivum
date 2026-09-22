// A reader for the IANA time zone database in its compiled form (TZif,
// RFC 8536, versions 1 to 4), including the footer's POSIX TZ rule for
// instants past the last explicit transition. The database is vendored
// as the IANA release tarball, compiled with zic at generation time, and
// embedded in the binary as byte arrays (tools/tzdata/generate.py,
// modules/punchline/tzdata/); nothing is read at runtime and nothing is
// downloaded. Day assignment, pairing and every check take the local
// time this computes from the instant and the site zone; the device's
// own wall clock is kept and compared (Stage 6 rulings, item 1).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "archivum/status.h"

namespace archivum::punchline::tz {

struct LocalTimeType {
  std::int32_t utoff = 0;  // seconds east of UT
  bool is_dst = false;
  std::string abbrev;
};

// One rule of a POSIX TZ string: "Mm.w.d/time", "Jn/time" or "n/time".
struct RuleDate {
  enum class Kind { MonthWeekDay, JulianNoLeap, JulianZeroBased } kind = Kind::MonthWeekDay;
  int month = 0, week = 0, weekday = 0;  // Mm.w.d
  int day = 0;                           // Jn (1..365, no Feb 29) or n (0..365)
  std::int64_t time = 7200;              // seconds after local midnight; may be negative or > 24h (v3)
};

struct PosixRule {
  LocalTimeType std;
  std::optional<LocalTimeType> dst;
  RuleDate start, end;  // meaningful when dst is set
};

class Zone {
 public:
  // Parses one TZif file. Corrupt or unsupported input is an error, never
  // a guess.
  static Result<Zone> parse(std::span<const std::byte> data);
  // Parses a POSIX TZ string as found in a TZif footer (exposed for tests).
  static Result<PosixRule> parse_posix(const std::string& tz);

  struct Local {
    std::int64_t offset = 0;  // seconds east of UT in force at the instant
    bool is_dst = false;
    std::string abbrev;
  };
  // The offset in force at `instant` (seconds since the Unix epoch, UT).
  Local local_of(std::int64_t instant) const;
  // Every instant whose local wall clock reads `local_seconds` (seconds
  // since the epoch as if UT): none in a gap, one normally, two in a fold,
  // ascending.
  std::vector<std::int64_t> instants_of(std::int64_t local_seconds) const;
  // Every transition in [from, to): (instant, offset after, dst after, abbrev after).
  struct Transition {
    std::int64_t at = 0;
    Local after;
  };
  std::vector<Transition> transitions(std::int64_t from, std::int64_t to) const;

  int version() const { return version_; }
  std::size_t explicit_transitions() const { return transition_times_.size(); }
  bool has_footer_rule() const { return footer_.has_value(); }

 private:
  Local type_local(std::size_t type_index) const;
  Local rule_local(std::int64_t instant) const;
  std::int64_t rule_transition(int year, const RuleDate& d, std::int64_t offset_before) const;
  int version_ = 0;
  std::vector<std::int64_t> transition_times_;
  std::vector<std::uint8_t> transition_types_;
  std::vector<LocalTimeType> types_;
  std::optional<PosixRule> footer_;
};

// The embedded database: every zone of the vendored IANA release, by name.
struct EmbeddedZone {
  const char* name;
  const unsigned char* data;
  std::size_t size;
};
struct EmbeddedDatabase {
  const char* version;  // the IANA release, e.g. "2025b"; "" when no database is embedded
  const EmbeddedZone* zones;
  std::size_t count;
};
const EmbeddedDatabase& embedded_database();

// Looks a zone up in the embedded database and parses it. NotFound for an
// unknown name; Unsupported when no database is embedded at all.
Result<Zone> zone(const std::string& name);
// The embedded release name, or an error when none is embedded.
Result<std::string> database_version();

}  // namespace archivum::punchline::tz
