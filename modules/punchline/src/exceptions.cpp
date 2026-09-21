#include "archivum/punchline/exceptions.h"

#include <algorithm>

#include "archivum/core/ids.h"

namespace archivum::punchline {
namespace {
using engine::Bound;
using engine::Row;
using engine::Value;

bool matches(const Exception& x, const ExceptionKey& k) {
  if (!k.kind.empty() && x.kind != k.kind) return false;
  return x.employee_id == k.employee_id && x.device_id == k.device_id && x.entry_id == k.entry_id && x.period_id == k.period_id;
}

Result<std::vector<Exception>> open_matching(engine::Reader& r, const ExceptionKey& key) {
  std::vector<Exception> out;
  Status s = r.scan("exceptions", "exceptions_state", Bound{{Value::text("open")}}, Bound{{Value::text("open")}}, false,
                    [&](const Row& row) {
                      Exception x = Exception::from_row(row);
                      if (matches(x, key)) out.push_back(std::move(x));
                      return true;
                    });
  if (!s.ok()) return s;
  return out;
}
}  // namespace

Result<Opened> open_exception(core::Recorder& rec, const ExceptionKey& key, const std::string& detail, std::int64_t now_us) {
  auto existing = open_matching(rec.writer(), key);
  if (!existing.ok()) return existing.status();
  if (!existing.value().empty()) return Opened{existing.value()[0].id, false};
  auto id = core::next_id(rec.writer(), "exceptions");
  if (!id.ok()) return id.status();
  Exception x;
  x.id = id.value();
  x.kind = key.kind;
  x.employee_id = key.employee_id;
  x.device_id = key.device_id;
  x.entry_id = key.entry_id;
  x.period_id = key.period_id;
  x.detail = detail;
  x.opened_at = now_us;
  if (Status s = rec.insert("exceptions", x.to_row()); !s.ok()) return s;
  return Opened{x.id, true};
}

Status close_exception(core::Recorder& rec, std::int64_t id, const std::string& new_state, const std::string& by, std::int64_t now_us) {
  auto x = exception_by_id(rec.writer(), id);
  if (!x.ok()) return x.status();
  if (!x.value().has_value()) return Status::not_found("no such exception");
  if (x.value()->state != "open") return Status::constraint("exception is already " + x.value()->state);
  Exception e = *x.value();
  e.state = new_state;
  e.resolved_at = now_us;
  e.resolved_by = by;
  e.resolution_audit_id = rec.audit_id();
  return rec.update("exceptions", e.to_row());
}

Result<int> resolve_matching(core::Recorder& rec, const ExceptionKey& key, const std::string& by, std::int64_t now_us) {
  auto open = open_matching(rec.writer(), key);
  if (!open.ok()) return open.status();
  for (const Exception& x : open.value()) {
    if (Status s = close_exception(rec, x.id, "resolved", by, now_us); !s.ok()) return s;
  }
  return static_cast<int>(open.value().size());
}

Result<std::vector<Exception>> open_exceptions(engine::Reader& r, const std::vector<std::int64_t>& employees, const std::string& kind) {
  std::vector<Exception> out;
  Status s = r.scan("exceptions", "exceptions_state", Bound{{Value::text("open")}}, Bound{{Value::text("open")}}, false,
                    [&](const Row& row) {
                      Exception x = Exception::from_row(row);
                      if (!kind.empty() && x.kind != kind) return true;
                      if (!employees.empty() && std::find(employees.begin(), employees.end(), x.employee_id) == employees.end()) return true;
                      out.push_back(std::move(x));
                      return true;
                    });
  if (!s.ok()) return s;
  return out;
}

}  // namespace archivum::punchline
