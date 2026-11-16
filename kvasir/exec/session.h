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
#include <vector>

#include "kvasir/exec/expression.h"
#include "kvasir/exec/variables.h"
#include "kvasir/sql/ast.h"
#include "kvasir/sql/parser.h"

// A client's session: its variables, user variables, current database and
// transaction, and the statements run against them. No tables exist yet, so
// a statement that names one is refused as MySQL refuses it when there is no
// such table: SELECT runs over no table (a select list, WHERE, LIMIT),
// SET sets variables, USE knows no database, and BEGIN, COMMIT and ROLLBACK
// mark the transaction a client sees in its status flags. Not thread-safe:
// one connection's strand owns it.
namespace kvasir::exec {

struct Column {
  std::string name;
  ResultType type;
};

struct ResultSet {
  std::vector<Column> columns;
  // Each value as the text protocol carries it; nothing for NULL.
  std::vector<std::vector<std::optional<std::string>>> rows;
};

// What a statement produced: rows, or an OK's counts.
struct Outcome {
  std::optional<ResultSet> result;
  uint64_t affected_rows = 0;
  uint64_t last_insert_id = 0;
};

struct SessionInfo {
  std::string user;
  std::string host;
  uint32_t connection_id = 0;
};

class Session : private VariableSource {
 public:
  Session(const VariableRegistry& registry, SessionInfo info);

  // Runs one statement. Throws SqlError; its warnings, and only its, are
  // then in warnings().
  Outcome execute(const sql::Statement& statement);

  // COM_RESET_CONNECTION: variables back to their defaults, user variables
  // gone, any transaction rolled back, no current database. Who is signed in
  // is kept.
  void reset();

  // A statement that failed before it reached the session (a syntax error,
  // an empty query): it fills the diagnostics area as an error does.
  void record_error(uint16_t code, const std::string& message);

  const std::vector<Warning>& warnings() const noexcept { return _warnings; }
  // SERVER_STATUS_IN_TRANS and SERVER_STATUS_AUTOCOMMIT as they stand.
  uint16_t status_flags() const;
  const std::optional<std::string>& database() const noexcept { return _database; }
  bool in_transaction() const noexcept { return _in_transaction; }
  // How the session's sql_mode has statements read.
  sql::ParserOptions parser_options() const;
  SessionVariables& variables() noexcept { return _variables; }
  const SessionInfo& info() const noexcept { return _info; }

 private:
  ResultType system_type(const std::string& scope, const std::string& name) const override;
  Value system_value(const std::string& scope, const std::string& name) const override;
  ResultType user_type(const std::string& name) const override;
  Value user_value(const std::string& name) const override;
  bool session_function(const std::string& name, ResultType& type, Value& value) const override;
  Outcome show_warnings(const sql::Statement& statement);

  EvalSettings eval_settings() const;
  Outcome run(const sql::Statement& statement);
  Outcome select(const sql::Select& select);
  void set(const std::vector<sql::Assignment>& assignments);
  void commit();

  struct UserVariable {
    Value value;
    ResultType type;
  };

  const VariableRegistry& _registry;
  SessionInfo _info;
  SessionVariables _variables;
  std::map<std::string, UserVariable> _user;
  std::optional<std::string> _database;
  bool _in_transaction = false;
  bool _read_only_transaction = false;
  std::vector<Warning> _warnings;
  // The diagnostics of the statement before the one running: what SHOW
  // WARNINGS lists and @@warning_count counts.
  std::vector<Warning> _previous;
  // ROW_COUNT() and FOUND_ROWS(): of the statement before.
  int64_t _row_count = -1;
  uint64_t _found_rows = 0;
};

inline constexpr uint16_t er_no_db_error = 1046;
inline constexpr uint16_t er_bad_db_error = 1049;
inline constexpr uint16_t er_no_tables_used = 1096;

}  // namespace kvasir::exec
