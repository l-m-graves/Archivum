#include "archivum/core/recorder.h"

#include "archivum/core/ids.h"
#include "archivum/core/schema.h"

namespace archivum::core {
namespace {

using engine::Row;
using engine::TableDef;
using engine::Value;

const char* kind_name(Value::Kind k) {
  switch (k) {
    case Value::Kind::Null:
      return "null";
    case Value::Kind::Integer:
      return "integer";
    case Value::Kind::Decimal:
      return "decimal";
    case Value::Kind::Text:
      return "text";
    case Value::Kind::Blob:
      return "blob";
    case Value::Kind::Timestamp:
      return "timestamp";
    case Value::Kind::Boolean:
      return "boolean";
    case Value::Kind::Uuid:
      return "uuid";
  }
  return "?";
}

nlohmann::json value_json(const Value& v) {
  switch (v.kind()) {
    case Value::Kind::Null:
      return nullptr;
    case Value::Kind::Integer:
    case Value::Kind::Decimal:
    case Value::Kind::Timestamp:
      return v.as_int64();
    case Value::Kind::Boolean:
      return v.as_bool();
    case Value::Kind::Text:
      return v.as_text();
    case Value::Kind::Uuid:
      return v.to_string();
    case Value::Kind::Blob:
      // Never by value.
      return nlohmann::json{{"kind", "blob"}, {"length", v.as_blob().size()}};
  }
  return nullptr;
}

nlohmann::json shape_json(const Value& v) {
  if (v.is_null()) return nlohmann::json{{"present", false}};
  nlohmann::json j{{"present", true}, {"kind", kind_name(v.kind())}};
  if (v.kind() == Value::Kind::Text) j["length"] = v.as_text().size();
  if (v.kind() == Value::Kind::Blob) j["length"] = v.as_blob().size();
  return j;
}

Actor::Kind kind_of(const Actor& a) { return a.kind; }

const char* actor_kind_name(Actor::Kind k) {
  switch (k) {
    case Actor::Kind::Principal:
      return "principal";
    case Actor::Kind::Device:
      return "device";
    case Actor::Kind::Account:
      return "account";
    case Actor::Kind::System:
      return "system";
  }
  return "system";
}

Value opt_text(const std::string& s) { return s.empty() ? Value::null() : Value::text(s); }

}  // namespace

void RecordPolicy::allow(const std::string& table, std::initializer_list<const char*> columns) {
  auto& set = recordable[table];
  for (const char* c : columns) set.insert(c);
}

bool RecordPolicy::allows(const std::string& table, const std::string& column) const {
  auto it = recordable.find(table);
  return it != recordable.end() && it->second.count(column) != 0;
}

nlohmann::json row_image(const TableDef& table, const Row& row, const RecordPolicy& policy) {
  nlohmann::json j = nlohmann::json::object();
  for (std::size_t i = 0; i < table.columns.size() && i < row.size(); ++i) {
    const std::string& name = table.columns[i].name;
    j[name] = policy.allows(table.name, name) ? value_json(row[i]) : shape_json(row[i]);
  }
  return j;
}

nlohmann::json row_key(const TableDef& table, const Row& row) {
  nlohmann::json j = nlohmann::json::object();
  for (const std::string& c : table.primary_key) j[c] = value_json(row[static_cast<std::size_t>(table.column_index(c))]);
  return j;
}

Recorder::Recorder(engine::Writer& writer, const RecordPolicy& policy, Actor actor, std::string action,
                   std::int64_t now_us, std::string detail)
    : writer_(writer), policy_(policy), actor_(std::move(actor)), now_us_(now_us) {
  auto id = next_id(writer_, kAuditLog);
  if (!id.ok()) {
    status_ = id.status();
    return;
  }
  audit_id_ = id.value();
  audit_row_ = Row(audit::kColumns);
  audit_row_[audit::kId] = Value::integer(audit_id_);
  audit_row_[audit::kAt] = Value::timestamp(now_us_);
  audit_row_[audit::kActorKind] = Value::text(actor_kind_name(kind_of(actor_)));
  audit_row_[audit::kActorTid] = opt_text(actor_.tid);
  audit_row_[audit::kActorOid] = opt_text(actor_.oid);
  audit_row_[audit::kActorAccount] = opt_text(actor_.account);
  audit_row_[audit::kActorDevice] = actor_.device ? Value::uuid(*actor_.device) : Value::null();
  audit_row_[audit::kAction] = Value::text(std::move(action));
  audit_row_[audit::kTargetTable] = Value::null();
  audit_row_[audit::kTargetId] = Value::null();
  audit_row_[audit::kRequestId] = opt_text(actor_.request_id);
  audit_row_[audit::kDetail] = opt_text(detail);
  status_ = writer_.insert(kAuditLog, audit_row_);
}

Status Recorder::set_target(const std::string& table, const std::string& id) {
  if (!status_.ok()) return status_;
  audit_row_[audit::kTargetTable] = Value::text(table);
  audit_row_[audit::kTargetId] = Value::text(id);
  return writer_.update(kAuditLog, audit_row_);
}

Status Recorder::feed(const std::string& table, const char* op, const nlohmann::json& key, const nlohmann::json* before,
                      const nlohmann::json* after) {
  auto id = next_id(writer_, kChangeFeed);
  if (!id.ok()) return id.status();
  Row r(feed::kColumns);
  r[feed::kId] = Value::integer(id.value());
  r[feed::kAuditId] = Value::integer(audit_id_);
  r[feed::kAt] = Value::timestamp(now_us_);
  r[feed::kTable] = Value::text(table);
  r[feed::kOp] = Value::text(op);
  r[feed::kKey] = Value::text(key.dump());
  r[feed::kBefore] = before ? Value::text(before->dump()) : Value::null();
  r[feed::kAfter] = after ? Value::text(after->dump()) : Value::null();
  return writer_.insert(kChangeFeed, r);
}

Status Recorder::insert(const std::string& table, const Row& row) {
  if (!status_.ok()) return status_;
  const TableDef* t = writer_.catalog().table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  if (Status s = writer_.insert(table, row); !s.ok()) return s;
  const nlohmann::json after = row_image(*t, row, policy_);
  return feed(table, "insert", row_key(*t, row), nullptr, &after);
}

Status Recorder::update(const std::string& table, const Row& row) {
  if (!status_.ok()) return status_;
  const TableDef* t = writer_.catalog().table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  Row pk;
  for (const std::string& c : t->primary_key) pk.push_back(row[static_cast<std::size_t>(t->column_index(c))]);
  auto old = writer_.get(table, pk);
  if (!old.ok()) return old.status();
  if (!old.value().has_value()) return Status::not_found("no row to update in " + table);
  if (Status s = writer_.update(table, row); !s.ok()) return s;
  const nlohmann::json before = row_image(*t, *old.value(), policy_);
  const nlohmann::json after = row_image(*t, row, policy_);
  return feed(table, "update", row_key(*t, row), &before, &after);
}

Status Recorder::remove(const std::string& table, const Row& primary_key) {
  if (!status_.ok()) return status_;
  const TableDef* t = writer_.catalog().table(table);
  if (t == nullptr) return Status::not_found("no table " + table);
  auto old = writer_.get(table, primary_key);
  if (!old.ok()) return old.status();
  if (!old.value().has_value()) return Status::not_found("no row to remove in " + table);
  if (Status s = writer_.remove(table, primary_key); !s.ok()) return s;
  const nlohmann::json before = row_image(*t, *old.value(), policy_);
  return feed(table, "delete", row_key(*t, *old.value()), &before, nullptr);
}

}  // namespace archivum::core
