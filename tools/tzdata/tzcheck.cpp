// Build-time assertion for the embedded time zone database (Stage 6
// rulings, item 1): the embedded bytes must reproduce every pinned
// transition (pinned-transitions.txt, produced by zdump at generation
// time), and, where zdump is on the build host, zdump run over the
// embedded bytes themselves must agree with the reader for every pinned
// zone over the pinned window. Any disagreement fails the build. With no
// database embedded it says so and fails too, unless --allow-empty is
// given (the placeholder before the IANA release is vendored).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "archivum/punchline/tzif.h"

using namespace archivum;
using namespace archivum::punchline::tz;

namespace {

int fail(const std::string& what) {
  std::fprintf(stderr, "tzcheck: %s\n", what.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string pinned_path;
  bool allow_empty = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--allow-empty") allow_empty = true;
    else if (a == "--pinned" && i + 1 < argc) pinned_path = argv[++i];
    else return fail("usage: tzcheck --pinned <file> [--allow-empty]");
  }
  const EmbeddedDatabase& db = embedded_database();
  if (db.count == 0) {
    std::fprintf(stderr, "tzcheck: NO TIME ZONE DATABASE IS EMBEDDED (placeholder); day assignment is not possible\n");
    return allow_empty ? 0 : 1;
  }
  std::ifstream in(pinned_path);
  if (!in) return fail("cannot read " + pinned_path);
  std::string line;
  int checked = 0;
  std::vector<std::string> problems;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string zone_name, abbrev;
    long long instant = 0, utoff = 0;
    int isdst = 0;
    if (!(ss >> zone_name >> instant >> utoff >> isdst >> abbrev)) return fail("bad pinned line: " + line);
    auto z = zone(zone_name);
    if (!z.ok()) return fail(zone_name + ": " + z.status().to_string());
    const Zone::Local after = z.value().local_of(instant);
    const Zone::Local before = z.value().local_of(instant - 1);
    if (after.offset != utoff || after.is_dst != (isdst != 0) || after.abbrev != abbrev) {
      problems.push_back(zone_name + " at " + std::to_string(instant) + ": embedded says utoff " + std::to_string(after.offset) +
                         " isdst " + std::to_string(after.is_dst) + " " + after.abbrev + ", zdump said " + std::to_string(utoff) + " " +
                         std::to_string(isdst) + " " + abbrev);
    }
    if (before.offset == after.offset && before.is_dst == after.is_dst && before.abbrev == after.abbrev) {
      problems.push_back(zone_name + " at " + std::to_string(instant) + ": zdump saw a transition, the embedded database does not");
    }
    ++checked;
  }
  if (checked == 0) return fail("no pinned transitions in " + pinned_path);
  for (const std::string& p : problems) std::fprintf(stderr, "tzcheck: %s\n", p.c_str());
  if (!problems.empty()) return 1;
  std::printf("tzcheck: %d pinned transitions reproduced by the embedded %s database (%zu zones)\n", checked, db.version, db.count);
  return 0;
}
