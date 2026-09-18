// The Punchline schema as migrations (docs/punchline-schema.md) and the
// module that owns it. Requires the core schema (audit_log) first.
#pragma once

#include <vector>

#include "archivum/core/module.h"
#include "archivum/engine/migrate.h"

namespace archivum::punchline {

constexpr const char* kModule = "punchline";

// Every migration of the Punchline module, in order.
const std::vector<engine::Migration>& migrations();

// Names of the tables migration 1 creates, in creation order.
const std::vector<std::string>& v1_tables();

class Module final : public core::Module {
 public:
  std::string name() const override { return kModule; }
  const std::vector<engine::Migration>& migrations() const override { return punchline::migrations(); }
  // Recordable by value: identifiers, states, times and codes. Never
  // recorded by value: credential hashes (blobs) and free text such as
  // notes, details and correction reasons, which are presence and shape.
  void extend_policy(core::RecordPolicy& policy) const override;
};

const Module& module();

}  // namespace archivum::punchline
