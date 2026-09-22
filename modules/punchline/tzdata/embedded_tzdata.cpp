// PLACEHOLDER: no time zone database is embedded. This file is replaced
// by tools/tzdata/generate.py from the vendored IANA release tarball
// (third_party/tzdata/). Until then tz::zone() answers Unsupported and the
// build-time check (tzcheck) reports the database as absent.
#include "archivum/punchline/tzif.h"

namespace archivum::punchline::tz {

const EmbeddedDatabase& embedded_database() {
  static const EmbeddedDatabase db{"", nullptr, 0};
  return db;
}

}  // namespace archivum::punchline::tz
