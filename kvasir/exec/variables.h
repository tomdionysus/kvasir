//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kvasir/exec/value.h"

// System variables: the registry of those Kvasir has, each with its scope,
// type, default and limits, and a session's values of them. The names,
// defaults, scopes and refusals are MySQL 8.4's, observed on MySQL 8.4.13,
// for the variables drivers and tools read as they connect; Kvasir adds
// ack_level. Names are compared without regard to case.
namespace kvasir::exec {

enum class VariableType : uint8_t {
  boolean,      // 0 or 1; set by 0, 1, ON, OFF, TRUE, FALSE
  unsigned_,    // an integer within [minimum, maximum], clamped with a warning
  string,       // any text
  enumeration,  // one of `choices`
  sql_mode,     // a set of sql_mode names, shown in MySQL's order
  charset,      // a character set's name
  collation,    // a collation's name
};

struct VariableSpec {
  std::string name;
  VariableType type = VariableType::string;
  // Where it lives: only global, or both global and session.
  bool has_session = true;
  // Never settable.
  bool read_only = false;
  // Settable only with SET GLOBAL (max_allowed_packet).
  bool session_read_only = false;
  // Its default: an integer for boolean and unsigned_, else text.
  Value initial;
  uint64_t minimum = 0;
  uint64_t maximum = UINT64_MAX;
  std::vector<std::string> choices;
  // May hold NULL (character_set_results).
  bool nullable = false;
};

// The variables a node has, and their global values. Built once at start;
// read-only after, so shared between sessions without a lock.
class VariableRegistry {
 public:
  // The MySQL 8.4 variables Kvasir serves, with `ack_level`'s default from
  // configuration and the version strings given.
  VariableRegistry(std::string ack_level, std::string version, std::string version_comment);

  const VariableSpec* find(std::string_view name) const;
  const std::vector<VariableSpec>& all() const noexcept { return _specs; }

 private:
  std::vector<VariableSpec> _specs;
};

// A session's values: its own copy of every variable that has a session
// value, and the global values beside them.
class SessionVariables {
 public:
  explicit SessionVariables(const VariableRegistry& registry);

  // @@[scope.]name as an expression reads it: scope is "", GLOBAL, SESSION
  // or LOCAL. Throws SqlError: 1193 for an unknown name, 1238 for a scope
  // the variable does not have.
  Value get(std::string_view scope, std::string_view name) const;
  ResultType type_of(std::string_view scope, std::string_view name, uint16_t result_collation, uint32_t result_max_bytes) const;

  // SET [scope] name = value. `value` is NULL for DEFAULT when `use_default`.
  // Throws SqlError: 1193 unknown, 1238 read only, 1229 global only, 1621
  // session read only, 1231 a value the variable cannot take, 1232 a value
  // of the wrong type; 1235 for SET GLOBAL and SET PERSIST, which a cluster
  // needs agreement for. A value clamped to the variable's range gives
  // warning 1292, appended to `warnings`.
  void set(std::string_view scope, std::string_view name, const Value& value, bool use_default, std::vector<Warning>& warnings);

  // The session value as text, for the session's own use (sql_mode,
  // collation_connection).
  std::string text(std::string_view name) const;
  uint64_t number(std::string_view name) const;

 private:
  const VariableSpec& spec(std::string_view name) const;

  const VariableRegistry& _registry;
  std::map<std::string, Value> _global;
  std::map<std::string, Value> _session;
};

// The collations Kvasir knows, by name: their id and character set.
struct CollationInfo {
  std::string name;
  uint16_t id = 0;
  std::string charset;
  bool is_default = false;
};
const CollationInfo* find_collation(std::string_view name);
const CollationInfo* default_collation(std::string_view charset);

}  // namespace kvasir::exec
