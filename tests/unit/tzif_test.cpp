// The TZif reader on synthetic fixtures built in the test: explicit
// transitions, the footer rule (both hemispheres, Julian forms, negative
// and over-24-hour times), the gap and the fold, corrupt input, and the
// empty placeholder database. The real database is asserted at build time
// against zdump by tools/tzdata/tzcheck; these tests hold with or without
// it.
#include <cstring>
#include <string>
#include <vector>

#include "archivum/punchline/tzif.h"
#include "test.h"

using namespace archivum;
using namespace archivum::punchline::tz;

namespace {

void put32(std::vector<std::byte>& v, std::uint32_t x) {
  for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::byte>((x >> (8 * i)) & 0xFF));
}
void put64(std::vector<std::byte>& v, std::int64_t x) {
  const auto u = static_cast<std::uint64_t>(x);
  for (int i = 7; i >= 0; --i) v.push_back(static_cast<std::byte>((u >> (8 * i)) & 0xFF));
}
void header(std::vector<std::byte>& v, char version, std::uint32_t timecnt, std::uint32_t typecnt, std::uint32_t charcnt) {
  for (char c : {'T', 'Z', 'i', 'f'}) v.push_back(static_cast<std::byte>(c));
  v.push_back(static_cast<std::byte>(version));
  for (int i = 0; i < 15; ++i) v.push_back(std::byte{0});
  put32(v, 0);  // isutcnt
  put32(v, 0);  // isstdcnt
  put32(v, 0);  // leapcnt
  put32(v, timecnt);
  put32(v, typecnt);
  put32(v, charcnt);
}
struct Type {
  std::int32_t utoff;
  bool dst;
  std::uint8_t abbr_index;
};
// A version-2 file: an empty v1 block (one type), then the real block with
// 64-bit transitions, then the footer.
std::vector<std::byte> tzif(const std::vector<std::pair<std::int64_t, std::uint8_t>>& transitions, const std::vector<Type>& types,
                            const std::string& chars, const std::string& footer) {
  std::vector<std::byte> v;
  header(v, '2', 0, 1, 1);
  put32(v, 0);
  v.push_back(std::byte{0});
  v.push_back(std::byte{0});
  v.push_back(std::byte{0});
  header(v, '2', static_cast<std::uint32_t>(transitions.size()), static_cast<std::uint32_t>(types.size()),
         static_cast<std::uint32_t>(chars.size()));
  for (const auto& [t, idx] : transitions) put64(v, t);
  for (const auto& [t, idx] : transitions) v.push_back(static_cast<std::byte>(idx));
  for (const Type& t : types) {
    put32(v, static_cast<std::uint32_t>(t.utoff));
    v.push_back(static_cast<std::byte>(t.dst ? 1 : 0));
    v.push_back(static_cast<std::byte>(t.abbr_index));
  }
  for (char c : chars) v.push_back(static_cast<std::byte>(c));
  v.push_back(std::byte{'\n'});
  for (char c : footer) v.push_back(static_cast<std::byte>(c));
  v.push_back(std::byte{'\n'});
  return v;
}

constexpr std::int64_t kPst = -28800, kPdt = -25200;
// The 2026 Pacific transitions, as zdump prints them: 2026-03-08 10:00:00
// UT (02:00 PST -> 03:00 PDT) and 2026-11-01 09:00:00 UT (02:00 PDT ->
// 01:00 PST). Definitional under M3.2.0 and M11.1.0.
constexpr std::int64_t kSpring2026 = 1'772'964'000;
constexpr std::int64_t kFall2026 = 1'793'523'600;
// And 2020's, as explicit transitions in the fixture.
constexpr std::int64_t kSpring2020 = 1'583'661'600;  // 2020-03-08 10:00:00 UT
constexpr std::int64_t kFall2020 = 1'604'221'200;    // 2020-11-01 09:00:00 UT

std::vector<std::byte> pacific() {
  return tzif({{kSpring2020, 1}, {kFall2020, 0}}, {{kPst, false, 0}, {kPdt, true, 4}}, std::string("PST\0PDT\0", 8),
              "PST8PDT,M3.2.0,M11.1.0");
}

}  // namespace

ARCHIVUM_TEST(tzif_explicit_transitions_and_footer_rule_agree_with_zdump_values) {
  auto z = Zone::parse(pacific());
  REQUIRE_OK(z.status());
  CHECK(z.value().version() == 2 && z.value().explicit_transitions() == 2 && z.value().has_footer_rule());
  // Before the first transition: the first non-DST type.
  CHECK(z.value().local_of(kSpring2020 - 1).offset == kPst);
  CHECK(z.value().local_of(kSpring2020).offset == kPdt && z.value().local_of(kSpring2020).is_dst);
  CHECK(z.value().local_of(kFall2020 - 1).offset == kPdt);
  CHECK(z.value().local_of(kFall2020).offset == kPst && z.value().local_of(kFall2020).abbrev == "PST");
  // Past the last explicit transition the footer rule governs: 2026.
  CHECK(z.value().local_of(kSpring2026 - 1).offset == kPst);
  CHECK(z.value().local_of(kSpring2026).offset == kPdt);
  CHECK(z.value().local_of(kFall2026 - 1).offset == kPdt);
  CHECK(z.value().local_of(kFall2026).offset == kPst);
  CHECK(z.value().local_of(kFall2026).abbrev == "PST" && z.value().local_of(kSpring2026).abbrev == "PDT");
  // transitions() lists both the explicit and the rule ones, once each.
  auto ts = z.value().transitions(kSpring2020 - 86400, kFall2026 + 86400);
  REQUIRE(ts.size() == 14);  // 2020 to 2026, two a year
  CHECK(ts[0].at == kSpring2020 && ts[1].at == kFall2020);
  CHECK(ts[12].at == kSpring2026 && ts[12].after.offset == kPdt);
  CHECK(ts[13].at == kFall2026 && ts[13].after.offset == kPst);
}

ARCHIVUM_TEST(tzif_gap_and_fold_resolve_through_the_instant) {
  auto z = Zone::parse(pacific());
  REQUIRE_OK(z.status());
  const Zone& zone = z.value();
  // 2026-03-08 02:30 local does not exist: no instant.
  const std::int64_t spring_local = kSpring2026 + kPst + 1800;  // 02:30 as if UT
  CHECK(zone.instants_of(spring_local).empty());
  // 01:30 exists once (PST), 03:30 exists once (PDT).
  CHECK(zone.instants_of(spring_local - 3600) == std::vector<std::int64_t>({kSpring2026 - 1800}));
  CHECK(zone.instants_of(spring_local + 3600) == std::vector<std::int64_t>({kSpring2026 + 1800}));
  // 2026-11-01 01:30 local exists twice: first PDT, then PST an hour later.
  const std::int64_t fall_local = kFall2026 + kPst + 1800;  // 01:30 as if UT
  auto both = zone.instants_of(fall_local);
  REQUIRE(both.size() == 2);
  CHECK(both[0] == kFall2026 - 1800 && zone.local_of(both[0]).is_dst);
  CHECK(both[1] == kFall2026 + 1800 && !zone.local_of(both[1]).is_dst);
  CHECK(both[1] - both[0] == 3600);
  // The instant resolves the ambiguity: a device that recorded 01:30 twice
  // with two instants an hour apart is two distinct punches.
  CHECK(zone.local_of(both[0]).offset == kPdt && zone.local_of(both[1]).offset == kPst);
  // A normal time, once.
  CHECK(zone.instants_of(fall_local + 86400).size() == 1);
}

ARCHIVUM_TEST(tzif_posix_rules_cover_both_hemispheres_and_every_date_form) {
  // Southern hemisphere: DST spans the new year.
  auto syd = Zone::parse(tzif({}, {{36000, false, 0}}, std::string("AEST\0", 5), "AEST-10AEDT,M10.1.0,M4.1.0/3"));
  REQUIRE_OK(syd.status());
  const std::int64_t jan15_2026 = 1'768'435'200;  // 2026-01-15 00:00:00 UT
  const std::int64_t jun15_2026 = 1'781'481'600;
  CHECK(syd.value().local_of(jan15_2026).offset == 39600 && syd.value().local_of(jan15_2026).abbrev == "AEDT");
  CHECK(syd.value().local_of(jun15_2026).offset == 36000 && !syd.value().local_of(jun15_2026).is_dst);
  // 2026-04-05 03:00 AEDT = 2026-04-04 16:00 UT ends DST; 2026-10-04 02:00 AEST = 2026-10-03 16:00 UT starts it.
  const std::int64_t end_2026 = 1'775'318'400, start_2026 = 1'791'043'200;
  CHECK(syd.value().local_of(end_2026 - 1).is_dst && !syd.value().local_of(end_2026).is_dst);
  CHECK(!syd.value().local_of(start_2026 - 1).is_dst && syd.value().local_of(start_2026).is_dst);
  // Quoted names, explicit dst offset, Julian forms, negative and long times.
  auto r1 = Zone::parse_posix("<+03>-3");
  REQUIRE_OK(r1.status());
  CHECK(r1.value().std.utoff == 10800 && r1.value().std.abbrev == "+03" && !r1.value().dst);
  auto r2 = Zone::parse_posix("IST-1GMT0,M10.5.0,M3.5.0/1");  // Europe/Dublin's inverted form
  REQUIRE_OK(r2.status());
  CHECK(r2.value().std.utoff == 3600 && r2.value().dst->utoff == 0);
  auto r3 = Zone::parse_posix("EST5EDT,J60/2,J300/2");
  REQUIRE_OK(r3.status());
  CHECK(r3.value().start.kind == RuleDate::Kind::JulianNoLeap && r3.value().start.day == 60);
  auto r4 = Zone::parse_posix("XXX3YYY,0/-1,364/25");
  REQUIRE_OK(r4.status());
  CHECK(r4.value().start.kind == RuleDate::Kind::JulianZeroBased && r4.value().start.time == -3600 && r4.value().end.time == 90000);
  // Jn never counts Feb 29: J60 is March 1 in a leap year too.
  auto jz = Zone::parse(tzif({}, {{-18000, false, 0}}, std::string("EST\0", 4), "EST5EDT,J60/2,J300/2"));
  REQUIRE_OK(jz.status());
  const std::int64_t mar1_2024_0700ut = 1'709'276'400;  // 2024-03-01 02:00 EST
  CHECK(!jz.value().local_of(mar1_2024_0700ut - 1).is_dst && jz.value().local_of(mar1_2024_0700ut).is_dst);
  // Errors.
  CHECK(!Zone::parse_posix("PS8").ok());
  CHECK(!Zone::parse_posix("PST8PDT").ok());
  CHECK(!Zone::parse_posix("PST8PDT,M13.1.0,M11.1.0").ok());
  CHECK(!Zone::parse_posix("PST8PDT,M3.2.0,M11.1.0x").ok());
}

ARCHIVUM_TEST(tzif_corrupt_input_is_refused) {
  auto good = pacific();
  auto bad_magic = good;
  bad_magic[0] = std::byte{'X'};
  CHECK(Zone::parse(bad_magic).status().code() == ErrorCode::Corrupt);
  CHECK(Zone::parse(std::span<const std::byte>(good.data(), 30)).status().code() == ErrorCode::Corrupt);
  CHECK(Zone::parse(std::span<const std::byte>(good.data(), good.size() - 10)).status().code() == ErrorCode::Corrupt);
  auto reversed = tzif({{kFall2020, 0}, {kSpring2020, 1}}, {{kPst, false, 0}, {kPdt, true, 4}}, std::string("PST\0PDT\0", 8), "PST8PDT,M3.2.0,M11.1.0");
  CHECK(Zone::parse(reversed).status().code() == ErrorCode::Corrupt);
  auto bad_type = tzif({{kSpring2020, 7}}, {{kPst, false, 0}}, std::string("PST\0", 4), "PST8");
  CHECK(Zone::parse(bad_type).status().code() == ErrorCode::Corrupt);
  auto bad_footer = tzif({}, {{kPst, false, 0}}, std::string("PST\0", 4), "PST8PDT,M3.2.0");
  CHECK(Zone::parse(bad_footer).status().code() == ErrorCode::Corrupt);
  // A version-1 file (no footer) is accepted and stays on its last type.
  std::vector<std::byte> v1;
  header(v1, '\0', 1, 2, 8);
  put32(v1, static_cast<std::uint32_t>(kSpring2020));
  v1.push_back(std::byte{1});
  for (const Type& t : {Type{static_cast<std::int32_t>(kPst), false, 0}, Type{static_cast<std::int32_t>(kPdt), true, 4}}) {
    put32(v1, static_cast<std::uint32_t>(t.utoff));
    v1.push_back(static_cast<std::byte>(t.dst ? 1 : 0));
    v1.push_back(static_cast<std::byte>(t.abbr_index));
  }
  for (char c : std::string("PST\0PDT\0", 8)) v1.push_back(static_cast<std::byte>(c));
  auto z1 = Zone::parse(v1);
  REQUIRE_OK(z1.status());
  CHECK(z1.value().version() == 1 && !z1.value().has_footer_rule());
  CHECK(z1.value().local_of(kSpring2020 - 1).offset == kPst && z1.value().local_of(kFall2026).offset == kPdt);
}

ARCHIVUM_TEST(tzif_embedded_database_lookup_fails_closed_when_absent) {
  const EmbeddedDatabase& db = embedded_database();
  if (db.count == 0) {
    CHECK(zone("America/Los_Angeles").status().code() == ErrorCode::Unsupported);
    CHECK(database_version().status().code() == ErrorCode::Unsupported);
    return;
  }
  auto v = database_version();
  REQUIRE_OK(v.status());
  CHECK(v.value().size() == 5);
  CHECK(zone("Nowhere/Nothing").status().code() == ErrorCode::NotFound);
  auto utc = zone("UTC");
  REQUIRE_OK(utc.status());
  CHECK(utc.value().local_of(kSpring2026).offset == 0);
  auto la = zone("America/Los_Angeles");
  REQUIRE_OK(la.status());
  CHECK(la.value().local_of(kSpring2026 - 1).offset == kPst && la.value().local_of(kSpring2026).offset == kPdt);
  CHECK(la.value().local_of(kFall2026 - 1).offset == kPdt && la.value().local_of(kFall2026).offset == kPst);
  CHECK(la.value().instants_of(kFall2026 + kPst + 1800).size() == 2);
}
