// The recorder: audit writer and change feed as one mechanism.
//
// Every mutating request runs one business transaction (an engine Writer)
// and every row change inside it goes through a Recorder, which performs
// the change and, in the same transaction, writes the change-feed row
// (table, key, operation, before and after images) under one audit row
// (who, what, when, request). Commit or roll back together: an audit row
// without its change, or a change without its audit row, cannot exist.
//
// The Synthex presence-and-shape rule (docs/confidentiality-check.md) is
// applied here and nowhere else: a column's value appears in an image
// only if the policy declares it recordable for that table; every other
// column is recorded as presence and shape (kind and length) only. Blobs
// are never recorded by value.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "archivum/engine/store.h"

namespace archivum::core {

struct RecordPolicy {
  // table -> columns whose values may be recorded. A table absent here
  // records presence and shape for every column.
  std::map<std::string, std::set<std::string>> recordable;
  void allow(const std::string& table, std::initializer_list<const char*> columns);
  bool allows(const std::string& table, const std::string& column) const;
};

struct Actor {
  enum class Kind { Principal, Device, Account, System };
  Kind kind = Kind::System;
  std::string tid, oid;   // Principal
  std::string account;    // Account (local)
  std::optional<engine::UuidBytes> device;  // Device
  std::string request_id;
};

// The image of a row under the policy: a JSON object keyed by column.
nlohmann::json row_image(const engine::TableDef& table, const engine::Row& row, const RecordPolicy& policy);
// The key of a row: its primary key values as JSON (values always, since
// keys identify rows and are never Synthex data by the confidentiality rule).
nlohmann::json row_key(const engine::TableDef& table, const engine::Row& row);

class Recorder {
 public:
  // Writes the audit row at construction; `action` names what the request
  // does (e.g. "device.enroll"). `target` may be filled in later.
  Recorder(engine::Writer& writer, const RecordPolicy& policy, Actor actor, std::string action, std::int64_t now_us,
           std::string detail = "");

  Status status() const { return status_; }  // of the audit row write
  std::int64_t audit_id() const { return audit_id_; }
  engine::Writer& writer() { return writer_; }

  // Row changes: performed and recorded. The writer's constraints apply
  // as usual; a refused change records nothing.
  Status insert(const std::string& table, const engine::Row& row);
  Status update(const std::string& table, const engine::Row& row);
  Status remove(const std::string& table, const engine::Row& primary_key);

  // Names the audit row's target (table and key) once known.
  Status set_target(const std::string& table, const std::string& id);

 private:
  Status feed(const std::string& table, const char* op, const nlohmann::json& key, const nlohmann::json* before,
              const nlohmann::json* after);

  engine::Writer& writer_;
  const RecordPolicy& policy_;
  Actor actor_;
  std::int64_t now_us_;
  std::int64_t audit_id_ = 0;
  Status status_;
  engine::Row audit_row_;
};

}  // namespace archivum::core
