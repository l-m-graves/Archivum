// Authorisation data: roles and dataset grants keyed on the Entra
// principal (tid, oid). Email never authorizes. A principal with no
// grant and no module identity is unknown: the caller answers 403 and
// audits it.
#pragma once

#include <set>
#include <string>

#include "archivum/core/recorder.h"
#include "archivum/engine/store.h"

namespace archivum::core {

Result<std::set<std::string>> roles_for(engine::Reader& reader, const std::string& tid, const std::string& oid);
Result<std::set<std::string>> datasets_for(engine::Reader& reader, const std::string& tid, const std::string& oid,
                                           const std::string& permission);

// Through a Recorder so the grant is audited with its actor.
Result<std::int64_t> grant_role(Recorder& rec, const std::string& tid, const std::string& oid, const std::string& role,
                                const std::string& granted_by, std::int64_t now_us);
Status revoke_role(Recorder& rec, std::int64_t grant_id);
Result<std::int64_t> grant_dataset(Recorder& rec, const std::string& tid, const std::string& oid, const std::string& dataset,
                                   const std::string& permission, const std::string& granted_by, std::int64_t now_us);
Status revoke_dataset(Recorder& rec, std::int64_t grant_id);

}  // namespace archivum::core
