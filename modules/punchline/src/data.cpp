#include "archivum/punchline/data.h"

namespace archivum::punchline {
namespace {
using engine::Bound;
using engine::Row;
using engine::Value;

Value opt_text(const std::string& s) { return s.empty() ? Value::null() : Value::text(s); }
Value opt_ts(std::int64_t t) { return t == 0 ? Value::null() : Value::timestamp(t); }
std::string text_or_empty(const Value& v) { return v.is_null() ? std::string() : v.as_text(); }
std::int64_t int_or_zero(const Value& v) { return v.is_null() ? 0 : v.as_int64(); }

template <class T>
Result<std::optional<T>> first(Result<std::vector<Row>> rows) {
  if (!rows.ok()) return rows.status();
  if (rows.value().empty()) return std::optional<T>();
  return std::optional<T>(T::from_row(rows.value()[0]));
}
}  // namespace

Row Employee::to_row() const {
  return {Value::integer(id), Value::text(employee_number), Value::text(display_name), opt_text(email), opt_text(tid),
          opt_text(oid), Value::boolean(active), opt_text(pay_group), Value::text(site_zone), Value::timestamp(created_at),
          Value::timestamp(updated_at)};
}
Employee Employee::from_row(const Row& r) {
  Employee e;
  e.id = r[0].as_int64();
  e.employee_number = r[1].as_text();
  e.display_name = r[2].as_text();
  e.email = text_or_empty(r[3]);
  e.tid = text_or_empty(r[4]);
  e.oid = text_or_empty(r[5]);
  e.active = r[6].as_bool();
  e.pay_group = text_or_empty(r[7]);
  e.site_zone = r[8].as_text();
  e.created_at = r[9].as_int64();
  e.updated_at = r[10].as_int64();
  return e;
}

Row Device::to_row() const {
  return {Value::integer(id), Value::uuid(device_uuid), Value::text(name), Value::integer(employee_id),
          Value::blob(credential_hash), Value::timestamp(enrolled_at), Value::text(enrolled_by), opt_ts(revoked_at),
          opt_text(revoked_reason), opt_ts(last_seen_at),
          last_acked_sequence < 0 ? Value::null() : Value::integer(last_acked_sequence)};
}
Device Device::from_row(const Row& r) {
  Device d;
  d.id = r[0].as_int64();
  d.device_uuid = r[1].as_uuid();
  d.name = r[2].as_text();
  d.employee_id = r[3].as_int64();
  d.credential_hash = r[4].as_blob();
  d.enrolled_at = r[5].as_int64();
  d.enrolled_by = r[6].as_text();
  d.revoked_at = int_or_zero(r[7]);
  d.revoked_reason = text_or_empty(r[8]);
  d.last_seen_at = int_or_zero(r[9]);
  d.last_acked_sequence = r[10].is_null() ? -1 : r[10].as_int64();
  return d;
}

Result<std::optional<Employee>> employee_by_id(engine::Reader& r, std::int64_t id) {
  auto row = r.get("employees", {Value::integer(id)});
  if (!row.ok()) return row.status();
  if (!row.value().has_value()) return std::optional<Employee>();
  return std::optional<Employee>(Employee::from_row(*row.value()));
}
Result<std::optional<Employee>> employee_by_number(engine::Reader& r, const std::string& number) {
  return first<Employee>(r.scan_all("employees", "employees_number", Bound{{Value::text(number)}}, Bound{{Value::text(number)}}));
}
Result<std::optional<Employee>> employee_by_identity(engine::Reader& r, const std::string& tid, const std::string& oid) {
  if (tid.empty() || oid.empty()) return std::optional<Employee>();
  return first<Employee>(r.scan_all("employees", "employees_identity", Bound{{Value::text(tid), Value::text(oid)}},
                                    Bound{{Value::text(tid), Value::text(oid)}}));
}
Result<std::optional<Device>> device_by_uuid(engine::Reader& r, const engine::UuidBytes& uuid) {
  return first<Device>(r.scan_all("devices", "devices_uuid", Bound{{Value::uuid(uuid)}}, Bound{{Value::uuid(uuid)}}));
}
Result<std::optional<Device>> device_by_id(engine::Reader& r, std::int64_t id) {
  auto row = r.get("devices", {Value::integer(id)});
  if (!row.ok()) return row.status();
  if (!row.value().has_value()) return std::optional<Device>();
  return std::optional<Device>(Device::from_row(*row.value()));
}

}  // namespace archivum::punchline
