#include "archivum/core/accounts.h"

#include "archivum/core/credentials.h"
#include "archivum/core/ids.h"
#include "archivum/core/schema.h"

namespace archivum::core {
namespace {

using engine::Bound;
using engine::Row;
using engine::Value;

Result<std::optional<Row>> find_account(engine::Reader& reader, const std::string& username) {
  auto rows = reader.scan_all(kLocalAccounts, "local_accounts_username", Bound{{Value::text(username)}},
                              Bound{{Value::text(username)}});
  if (!rows.ok()) return rows.status();
  if (rows.value().empty()) return std::optional<Row>();
  return std::optional<Row>(rows.value()[0]);
}

Actor console() {
  Actor a;
  a.kind = Actor::Kind::System;
  a.account = "console";
  return a;
}

}  // namespace

LocalAccount account_from_row(const Row& r) {
  LocalAccount a;
  a.id = r[accounts::kId].as_int64();
  a.username = r[accounts::kUsername].as_text();
  a.kind = r[accounts::kKind].as_text();
  a.enabled = r[accounts::kEnabled].as_bool();
  a.created_at = r[accounts::kCreatedAt].as_int64();
  a.expires_at = r[accounts::kExpiresAt].is_null() ? 0 : r[accounts::kExpiresAt].as_int64();
  a.last_used_at = r[accounts::kLastUsedAt].is_null() ? 0 : r[accounts::kLastUsedAt].as_int64();
  return a;
}

Result<CreatedAccount> create_account(engine::Store& store, const RecordPolicy& policy, const std::string& username,
                                      const std::string& kind, std::int64_t now_us) {
  if (username.empty()) return Status::invalid_argument("username is empty");
  auto w = store.begin_write();
  if (!w.ok()) return w.status();
  auto existing = find_account(*w.value(), username);
  if (!existing.ok()) return existing.status();
  if (existing.value().has_value()) return Status::already_exists("account " + username);
  auto id = next_id(*w.value(), kLocalAccounts);
  if (!id.ok()) return id.status();
  CreatedAccount out;
  out.password = random_secret(24);
  auto verifier = make_verifier(out.password);
  if (!verifier.ok()) return verifier.status();
  Row r(accounts::kColumns);
  r[accounts::kId] = Value::integer(id.value());
  r[accounts::kUsername] = Value::text(username);
  r[accounts::kKind] = Value::text(kind);
  r[accounts::kVerifier] = Value::blob(verifier.value());
  r[accounts::kEnabled] = Value::boolean(false);  // disabled by default (Q9)
  r[accounts::kCreatedAt] = Value::timestamp(now_us);
  r[accounts::kExpiresAt] = Value::null();
  r[accounts::kLastUsedAt] = Value::null();
  Recorder rec(*w.value(), policy, console(), "account.create", now_us);
  if (!rec.status().ok()) return rec.status();
  if (Status s = rec.insert(kLocalAccounts, r); !s.ok()) return s;
  if (Status s = rec.set_target(kLocalAccounts, std::to_string(id.value())); !s.ok()) return s;
  if (Status s = w.value()->commit(); !s.ok()) return s;
  out.account = account_from_row(r);
  return out;
}

Status enable_account(engine::Store& store, const RecordPolicy& policy, const std::string& username,
                      std::int64_t expires_at_us, std::int64_t now_us) {
  if (expires_at_us <= now_us) return Status::invalid_argument("expiry must be in the future");
  auto w = store.begin_write();
  if (!w.ok()) return w.status();
  auto found = find_account(*w.value(), username);
  if (!found.ok()) return found.status();
  if (!found.value().has_value()) return Status::not_found("account " + username);
  Row r = *found.value();
  r[accounts::kEnabled] = Value::boolean(true);
  r[accounts::kExpiresAt] = Value::timestamp(expires_at_us);
  Recorder rec(*w.value(), policy, console(), "account.enable", now_us, "expires_at=" + std::to_string(expires_at_us));
  if (!rec.status().ok()) return rec.status();
  if (Status s = rec.update(kLocalAccounts, r); !s.ok()) return s;
  if (Status s = rec.set_target(kLocalAccounts, std::to_string(r[accounts::kId].as_int64())); !s.ok()) return s;
  return w.value()->commit();
}

Status disable_account(engine::Store& store, const RecordPolicy& policy, const std::string& username, std::int64_t now_us) {
  auto w = store.begin_write();
  if (!w.ok()) return w.status();
  auto found = find_account(*w.value(), username);
  if (!found.ok()) return found.status();
  if (!found.value().has_value()) return Status::not_found("account " + username);
  Row r = *found.value();
  r[accounts::kEnabled] = Value::boolean(false);
  Recorder rec(*w.value(), policy, console(), "account.disable", now_us);
  if (!rec.status().ok()) return rec.status();
  if (Status s = rec.update(kLocalAccounts, r); !s.ok()) return s;
  if (Status s = rec.set_target(kLocalAccounts, std::to_string(r[accounts::kId].as_int64())); !s.ok()) return s;
  return w.value()->commit();
}

Result<std::vector<LocalAccount>> list_accounts(engine::Reader& reader) {
  auto rows = reader.scan_all(kLocalAccounts);
  if (!rows.ok()) return rows.status();
  std::vector<LocalAccount> out;
  for (const Row& r : rows.value()) out.push_back(account_from_row(r));
  return out;
}

Result<LocalAccount> verify_account(engine::Store& store, const RecordPolicy& policy, const std::string& username,
                                    const std::string& password, const std::string& request_id, std::int64_t now_us) {
  auto w = store.begin_write();
  if (!w.ok()) return w.status();
  auto found = find_account(*w.value(), username);
  if (!found.ok()) return found.status();
  if (!found.value().has_value()) return Status::not_found("unknown account");
  Row r = *found.value();
  LocalAccount a = account_from_row(r);
  if (!a.enabled) return Status::not_found("account disabled");
  if (a.expires_at != 0 && a.expires_at <= now_us) {
    // Auto-disable: the expiry is enforced by the store, not by memory.
    r[accounts::kEnabled] = Value::boolean(false);
    Recorder rec(*w.value(), policy, console(), "account.expire", now_us);
    if (!rec.status().ok()) return rec.status();
    if (Status s = rec.update(kLocalAccounts, r); !s.ok()) return s;
    if (Status s = rec.set_target(kLocalAccounts, std::to_string(a.id)); !s.ok()) return s;
    if (Status s = w.value()->commit(); !s.ok()) return s;
    return Status::not_found("account expired");
  }
  if (!verify_secret(r[accounts::kVerifier].as_blob(), password)) return Status::invalid_argument("wrong password");
  r[accounts::kLastUsedAt] = Value::timestamp(now_us);
  Actor actor;
  actor.kind = Actor::Kind::Account;
  actor.account = username;
  actor.request_id = request_id;
  Recorder rec(*w.value(), policy, actor, "account.use", now_us);
  if (!rec.status().ok()) return rec.status();
  if (Status s = rec.update(kLocalAccounts, r); !s.ok()) return s;
  if (Status s = rec.set_target(kLocalAccounts, std::to_string(a.id)); !s.ok()) return s;
  if (Status s = w.value()->commit(); !s.ok()) return s;
  return account_from_row(r);
}

}  // namespace archivum::core
