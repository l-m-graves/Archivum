// The core library: recorder (audit writer and change feed as one
// mechanism, the presence-and-shape rule with a canary scan), local
// accounts, roles and dataset grants.
#include <cstring>

#include "archivum/core/accounts.h"
#include "archivum/core/authz.h"
#include "archivum/core/credentials.h"
#include "archivum/core/ids.h"
#include "archivum/core/module.h"
#include "archivum/core/recorder.h"
#include "archivum/core/schema.h"
#include "archivum/testing/mem_vfs.h"
#include "test.h"

using namespace archivum;
using namespace archivum::core;
using namespace archivum::engine;
using namespace archivum::testing;

namespace {

// A stand-in for a Synthex table: undeclared columns carrying data that
// must never reach the audit log or the change feed.
class SynthexLike final : public core::Module {
 public:
  std::string name() const override { return "synthexlike"; }
  const std::vector<Migration>& migrations() const override {
    static const std::vector<Migration> list = {Migration{1, "synthexlike_v1", [](Writer& w) {
      TableDef t;
      t.name = "samples";
      t.columns = {{"id", ColumnType::Integer, false, 0},
                   {"label", ColumnType::Text, false, 0},
                   {"payload", ColumnType::Text, true, 0},
                   {"raw", ColumnType::Blob, true, 0},
                   {"count", ColumnType::Integer, true, 0}};
      t.primary_key = {"id"};
      return w.create_table(t);
    }}};
    return list;
  }
  // Only the label is declared recordable; payload, raw and count are not.
  void extend_policy(RecordPolicy& p) const override { p.allow("samples", {"id", "label"}); }
};

const char* kCanary = "CANARY-7f3a9c-original-data";

Bytes bytes_of(const std::string& s) {
  Bytes b(s.size());
  std::memcpy(b.data(), s.data(), s.size());
  return b;
}

}  // namespace

ARCHIVUM_TEST(recorder_writes_audit_and_feed_in_one_transaction_under_the_policy) {
  MemVfs vfs;
  auto st = Store::open(vfs, "c/rec.db");
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  SynthexLike synthex;
  REQUIRE_OK(migrate_all(store, {&synthex}).status());
  const RecordPolicy policy = build_policy({&synthex});
  Actor actor;
  actor.kind = Actor::Kind::Principal;
  actor.tid = "tid-1";
  actor.oid = "oid-1";
  actor.request_id = "req-1";
  {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    Recorder rec(*w.value(), policy, actor, "samples.load", 1000);
    REQUIRE_OK(rec.status());
    REQUIRE_OK(rec.insert("samples", {Value::integer(1), Value::text("first"), Value::text(kCanary),
                                      Value::blob(bytes_of(kCanary)), Value::integer(42)}));
    REQUIRE_OK(rec.update("samples", {Value::integer(1), Value::text("first-renamed"), Value::text(std::string(kCanary) + "-v2"),
                                      Value::null(), Value::integer(43)}));
    REQUIRE_OK(rec.insert("samples", {Value::integer(2), Value::text("second"), Value::null(), Value::null(), Value::null()}));
    REQUIRE_OK(rec.remove("samples", {Value::integer(2)}));
    REQUIRE_OK(rec.set_target("samples", "1"));
    // A refused change records nothing.
    CHECK(rec.insert("samples", {Value::integer(1), Value::text("dup"), Value::null(), Value::null(), Value::null()}).code() ==
          ErrorCode::Constraint);
    REQUIRE_OK(w.value()->commit());
  }
  auto rd = store.begin_read();
  REQUIRE_OK(rd.status());
  auto audit = rd.value()->scan_all(kAuditLog);
  REQUIRE_OK(audit.status());
  REQUIRE(audit.value().size() == 1);
  const Row& a = audit.value()[0];
  CHECK(a[audit::kActorKind] == Value::text("principal"));
  CHECK(a[audit::kActorOid] == Value::text("oid-1"));
  CHECK(a[audit::kAction] == Value::text("samples.load"));
  CHECK(a[audit::kTargetTable] == Value::text("samples"));
  CHECK(a[audit::kRequestId] == Value::text("req-1"));
  auto feed_rows = rd.value()->scan_all(kChangeFeed);
  REQUIRE_OK(feed_rows.status());
  REQUIRE(feed_rows.value().size() == 4);
  CHECK(feed_rows.value()[0][feed::kOp] == Value::text("insert"));
  CHECK(feed_rows.value()[1][feed::kOp] == Value::text("update"));
  CHECK(feed_rows.value()[3][feed::kOp] == Value::text("delete"));
  for (const Row& f : feed_rows.value()) CHECK(f[feed::kAuditId] == a[audit::kId]);
  // The declared column is recorded by value; the undeclared ones as
  // presence and shape; the blob never by value.
  auto after = nlohmann::json::parse(feed_rows.value()[0][feed::kAfter].as_text());
  CHECK(after["label"] == "first");
  CHECK(after["payload"]["present"] == true && after["payload"]["kind"] == "text" &&
        after["payload"]["length"] == std::strlen(kCanary));
  CHECK(!after["payload"].contains("value"));
  CHECK(after["raw"]["kind"] == "blob" && after["raw"]["length"] == std::strlen(kCanary));
  CHECK(after["count"]["present"] == true && !after["count"].contains("value"));
  auto before = nlohmann::json::parse(feed_rows.value()[1][feed::kBefore].as_text());
  CHECK(before["label"] == "first");
  auto after2 = nlohmann::json::parse(feed_rows.value()[1][feed::kAfter].as_text());
  CHECK(after2["raw"]["present"] == false);
  // Canary scan: the original data appears nowhere in the audit log or the feed.
  for (const Row& f : feed_rows.value()) {
    for (std::size_t i : {feed::kKey, feed::kBefore, feed::kAfter}) {
      if (f[i].is_null()) continue;
      CHECK_MSG(f[i].as_text().find("CANARY") == std::string::npos, "canary leaked into the change feed: " << f[i].as_text());
    }
  }
  for (const Value& v : a) {
    if (v.kind() == Value::Kind::Text) CHECK(v.as_text().find("CANARY") == std::string::npos);
  }
  // The row itself holds the data, as it must.
  auto row = rd.value()->get("samples", {Value::integer(1)});
  REQUIRE_OK(row.status());
  CHECK((*row.value())[2].as_text().find("CANARY") != std::string::npos);
  auto rep = store.check();
  REQUIRE_OK(rep.status());
  CHECK_MSG(rep.value().ok, rep.value().problems[0]);
}

ARCHIVUM_TEST(recorder_rollback_leaves_no_audit_row) {
  MemVfs vfs;
  auto st = Store::open(vfs, "c/rb.db");
  REQUIRE_OK(st.status());
  REQUIRE_OK(migrate_all(*st.value(), {}).status());
  const RecordPolicy policy = build_policy({});
  {
    auto w = st.value()->begin_write();
    REQUIRE_OK(w.status());
    Recorder rec(*w.value(), policy, Actor{}, "role.grant", 5);
    REQUIRE_OK(rec.status());
    REQUIRE_OK(grant_role(rec, "t", "o", "admin", "test", 5).status());
    w.value()->rollback();
  }
  auto rd = st.value()->begin_read();
  REQUIRE_OK(rd.status());
  CHECK(rd.value()->count(kAuditLog).value() == 0);
  CHECK(rd.value()->count(kChangeFeed).value() == 0);
  CHECK(rd.value()->count(kRoleGrants).value() == 0);
}

ARCHIVUM_TEST(local_accounts_are_disabled_by_default_expire_and_audit_every_use) {
  MemVfs vfs;
  auto st = Store::open(vfs, "c/acc.db");
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  REQUIRE_OK(migrate_all(store, {}).status());
  const RecordPolicy policy = build_policy({});
  auto created = create_account(store, policy, "breakglass", "break_glass", 1000);
  REQUIRE_OK(created.status());
  CHECK(!created.value().account.enabled);
  CHECK(created.value().password.size() >= 32);
  CHECK(create_account(store, policy, "breakglass", "break_glass", 1000).status().code() == ErrorCode::AlreadyExists);
  CHECK(create_account(store, policy, "x", "root", 1000).status().code() == ErrorCode::Constraint);
  // Disabled: refused even with the right password.
  CHECK(verify_account(store, policy, "breakglass", created.value().password, "r1", 2000).status().code() == ErrorCode::NotFound);
  REQUIRE_OK(enable_account(store, policy, "breakglass", 10'000, 2000));
  CHECK(verify_account(store, policy, "breakglass", "wrong", "r2", 3000).status().code() == ErrorCode::InvalidArgument);
  auto ok = verify_account(store, policy, "breakglass", created.value().password, "r3", 3000);
  REQUIRE_OK(ok.status());
  CHECK(ok.value().last_used_at == 3000);
  // Expired: disabled on the spot.
  CHECK(verify_account(store, policy, "breakglass", created.value().password, "r4", 20'000).status().code() == ErrorCode::NotFound);
  auto rd = store.begin_read();
  REQUIRE_OK(rd.status());
  auto list = list_accounts(*rd.value());
  REQUIRE_OK(list.status());
  REQUIRE(list.value().size() == 1);
  CHECK(!list.value()[0].enabled);
  auto audit = rd.value()->scan_all(kAuditLog);
  REQUIRE_OK(audit.status());
  std::vector<std::string> actions;
  for (const Row& r : audit.value()) actions.push_back(r[audit::kAction].as_text());
  CHECK(actions == std::vector<std::string>({"account.create", "account.enable", "account.use", "account.expire"}));
  // The verifier is never recorded by value.
  auto feed_all = rd.value()->scan_all(kChangeFeed);
  REQUIRE_OK(feed_all.status());
  for (const Row& f : feed_all.value()) {
    if (!f[feed::kAfter].is_null()) {
      auto j = nlohmann::json::parse(f[feed::kAfter].as_text());
      CHECK(j["verifier"].is_object() && j["verifier"]["kind"] == "blob");
    }
  }
  CHECK(verify_secret(make_verifier("s3cret").value(), "s3cret"));
  CHECK(!verify_secret(make_verifier("s3cret").value(), "s3cre"));
  CHECK(uuid_from_string(uuid_to_string(random_uuid())).ok());
  CHECK(!uuid_from_string("nope").ok());
}

ARCHIVUM_TEST(roles_and_dataset_grants_are_data) {
  MemVfs vfs;
  auto st = Store::open(vfs, "c/roles.db");
  REQUIRE_OK(st.status());
  Store& store = *st.value();
  REQUIRE_OK(migrate_all(store, {}).status());
  const RecordPolicy policy = build_policy({});
  std::int64_t sup = 0;
  {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    Recorder rec(*w.value(), policy, Actor{}, "role.grant", 1);
    auto a = grant_role(rec, "tid", "oid-9", "supervisor", "installer", 1);
    REQUIRE_OK(a.status());
    sup = a.value();
    REQUIRE_OK(grant_role(rec, "tid", "oid-9", "payroll", "installer", 1).status());
    CHECK(grant_role(rec, "tid", "oid-9", "payroll", "installer", 1).status().code() == ErrorCode::Constraint);
    CHECK(grant_role(rec, "tid", "oid-9", "root", "installer", 1).status().code() == ErrorCode::Constraint);
    REQUIRE_OK(grant_dataset(rec, "tid", "oid-9", "finalysis.gl", "read", "installer", 1).status());
    REQUIRE_OK(w.value()->commit());
  }
  {
    auto rd = store.begin_read();
    REQUIRE_OK(rd.status());
    auto roles = roles_for(*rd.value(), "tid", "oid-9");
    REQUIRE_OK(roles.status());
    CHECK(roles.value() == std::set<std::string>({"supervisor", "payroll"}));
    CHECK(roles_for(*rd.value(), "tid", "nobody").value().empty());
    CHECK(datasets_for(*rd.value(), "tid", "oid-9", "read").value().count("finalysis.gl") == 1);
    CHECK(datasets_for(*rd.value(), "tid", "oid-9", "write").value().empty());
  }
  {
    auto w = store.begin_write();
    REQUIRE_OK(w.status());
    Recorder rec(*w.value(), policy, Actor{}, "role.revoke", 2);
    REQUIRE_OK(revoke_role(rec, sup));
    REQUIRE_OK(w.value()->commit());
  }
  auto rd = store.begin_read();
  REQUIRE_OK(rd.status());
  CHECK(roles_for(*rd.value(), "tid", "oid-9").value() == std::set<std::string>({"payroll"}));
  CHECK(rd.value()->count(kAuditLog).value() == 2);
}
