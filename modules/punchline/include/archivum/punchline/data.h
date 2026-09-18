// Typed access to the Punchline tables the server needs in Stage 5:
// employees and devices. Column indexes are the declaration order in
// schema.cpp; these helpers keep that knowledge in one place.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "archivum/engine/store.h"

namespace archivum::punchline {

struct Employee {
  std::int64_t id = 0;
  std::string employee_number, display_name, email, tid, oid, pay_group, site_zone;
  bool active = true;
  std::int64_t created_at = 0, updated_at = 0;
  engine::Row to_row() const;
  static Employee from_row(const engine::Row& r);
};

struct Device {
  std::int64_t id = 0;
  engine::UuidBytes device_uuid{};
  std::string name;
  std::int64_t employee_id = 0;
  std::vector<std::byte> credential_hash;
  std::int64_t enrolled_at = 0;
  std::string enrolled_by;
  std::int64_t revoked_at = 0;  // 0: active
  std::string revoked_reason;
  std::int64_t last_seen_at = 0;
  std::int64_t last_acked_sequence = -1;  // -1: none
  engine::Row to_row() const;
  static Device from_row(const engine::Row& r);
};

Result<std::optional<Employee>> employee_by_id(engine::Reader& r, std::int64_t id);
Result<std::optional<Employee>> employee_by_number(engine::Reader& r, const std::string& number);
Result<std::optional<Employee>> employee_by_identity(engine::Reader& r, const std::string& tid, const std::string& oid);
Result<std::optional<Device>> device_by_uuid(engine::Reader& r, const engine::UuidBytes& uuid);
Result<std::optional<Device>> device_by_id(engine::Reader& r, std::int64_t id);

}  // namespace archivum::punchline
