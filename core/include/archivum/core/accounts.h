// Local accounts (instructions v2, Q9): the break-glass administrator and
// device-administration accounts. Created only by a console command on
// the host, disabled by default, enabled for a bounded time, every use
// audited. The password is generated, shown once, and never stored.
#pragma once

#include <cstdint>
#include <string>

#include "archivum/core/recorder.h"
#include "archivum/engine/store.h"

namespace archivum::core {

struct LocalAccount {
  std::int64_t id = 0;
  std::string username;
  std::string kind;  // break_glass | device_admin
  bool enabled = false;
  std::int64_t created_at = 0;
  std::int64_t expires_at = 0;    // 0: none
  std::int64_t last_used_at = 0;  // 0: never
};

struct CreatedAccount {
  LocalAccount account;
  std::string password;  // shown once
};

// All of these run their own write transaction and record through the
// Recorder as actor System (console). `now_us` is the caller's clock.
Result<CreatedAccount> create_account(engine::Store& store, const RecordPolicy& policy, const std::string& username,
                                      const std::string& kind, std::int64_t now_us);
Status enable_account(engine::Store& store, const RecordPolicy& policy, const std::string& username,
                      std::int64_t expires_at_us, std::int64_t now_us);
Status disable_account(engine::Store& store, const RecordPolicy& policy, const std::string& username, std::int64_t now_us);
Result<std::vector<LocalAccount>> list_accounts(engine::Reader& reader);

// Verifies a username and password. Success records last_used_at and an
// audit row (action "account.use") in its own transaction; an expired
// account is disabled on the spot. Failure is NotFound (unknown, disabled,
// expired) or InvalidArgument (wrong password); the caller audits it.
Result<LocalAccount> verify_account(engine::Store& store, const RecordPolicy& policy, const std::string& username,
                                    const std::string& password, const std::string& request_id, std::int64_t now_us);

LocalAccount account_from_row(const engine::Row& row);

}  // namespace archivum::core
