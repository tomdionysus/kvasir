//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kvasir/catalog/catalog.h"
#include "kvasir/exec/expression.h"
#include "kvasir/exec/variables.h"
#include "kvasir/sql/ast.h"
#include "kvasir/sql/parser.h"
#include "kvasir/txn/store.h"

// A client's session: its variables, user variables, current database and
// transaction, and the statements run against them. Databases and tables are
// the node's catalog's, and rows the node's store's, shared with every other
// session. SELECT runs over no table (a select list, WHERE, LIMIT) or over
// one table's rows (WHERE, ORDER BY, LIMIT); INSERT adds rows; SET sets
// variables; USE, CREATE and DROP DATABASE, SHOW DATABASES, CREATE and DROP
// TABLE and SHOW TABLES work on the catalog. Transactions are the store's:
// BEGIN or autocommit = 0 opens one, COMMIT and ROLLBACK end it, and with
// autocommit each statement is one; DDL commits first; a failed statement
// undoes only itself. REPEATABLE READ (a snapshot at the first read) and
// READ COMMITTED (a snapshot each statement) are served, READ UNCOMMITTED
// as READ COMMITTED; SERIALIZABLE is refused until read marks exist. Not
// thread-safe: one connection's strand owns it (the catalog and store are
// safe from any thread).
namespace kvasir::exec {

struct Column {
  Column() = default;
  Column(std::string column_name, ResultType column_type) : name(std::move(column_name)), type(column_type) {}

  std::string name;
  ResultType type;
  // A table's column: its database, the table as named in the statement
  // (its alias, if any), the table's own name and the column's.
  std::string schema;
  std::string table;
  std::string original_table;
  std::string original_name;
};

struct ResultSet {
  std::vector<Column> columns;
  // Each value as the text protocol carries it; nothing for NULL.
  std::vector<std::vector<std::optional<std::string>>> rows;
};

// What a statement produced: rows, or an OK's counts and its info text
// ("Records: 2  Duplicates: 0  Warnings: 0" after a multi-row INSERT).
struct Outcome {
  std::optional<ResultSet> result;
  uint64_t affected_rows = 0;
  uint64_t last_insert_id = 0;
  std::string info;
};

// A statement met a lock another transaction holds. It has been undone, and
// may be run again once that transaction lets its locks go (Session::
// wait_for_lock); nothing of it shows in the diagnostics.
class LockWaitNeeded : public std::exception {
 public:
  LockWaitNeeded(uint64_t waiter, uint64_t holder) : _waiter(waiter), _holder(holder) {}
  const char* what() const noexcept override { return "a lock wait is needed"; }
  uint64_t waiter() const noexcept { return _waiter; }
  uint64_t holder() const noexcept { return _holder; }

 private:
  uint64_t _waiter;
  uint64_t _holder;
};

inline constexpr uint16_t er_lock_wait_timeout = 1205;
inline constexpr uint16_t er_lock_deadlock = 1213;

struct SessionInfo {
  std::string user;
  std::string host;
  uint32_t connection_id = 0;
  // The collation the client asked for in its handshake; 0 for none.
  uint16_t client_collation = 0;
};

class Session : private VariableSource {
 public:
  // `catalog` and `rows` are the node's, and outlive the session.
  Session(const VariableRegistry& registry, catalog::Catalog& catalog, txn::Store& rows, SessionInfo info);
  // A session that ends rolls back its transaction, freeing its locks.
  ~Session();
  Session(Session&&) = default;
  Session& operator=(Session&&) = delete;

  // Runs one statement. Throws SqlError; its warnings, and only its, are
  // then in warnings(). Throws LockWaitNeeded for a statement that must wait.
  Outcome execute(const sql::Statement& statement);

  // After LockWaitNeeded: waits for the holder, `wake` being called once on
  // the thread that frees it (it must only post). False for a deadlock: this
  // session's transaction is rolled back whole, its error (1213) recorded.
  bool wait_for_lock(const LockWaitNeeded& wait, std::function<void()> wake);
  // The wait is given up: its error (1205) recorded; the transaction goes on.
  void give_up_waiting(const LockWaitNeeded& wait);
  // innodb_lock_wait_timeout, in seconds.
  uint64_t lock_wait_timeout() const;

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
  Outcome create_database(const sql::Statement& statement);
  Outcome drop_database(const sql::Statement& statement);
  Outcome show_databases(const sql::Statement& statement);
  Outcome create_table(const sql::Statement& statement);
  Outcome drop_table(const sql::Statement& statement);
  Outcome show_tables(const sql::Statement& statement);
  Outcome select_from_table(const sql::Select& select);
  // A SELECT with GROUP BY, HAVING or an aggregate, over `table` (none for
  // a SELECT without FROM: one row).
  Outcome select_groups(const sql::Select& select, const catalog::Table* table, const std::string& database);
  Outcome insert(const sql::Statement& statement);
  // UPDATE and DELETE.
  Outcome change_rows(const sql::Statement& statement);
  // The transaction a statement runs in: the open one, else a new one, which
  // autocommit commits when the statement ends.
  txn::Transaction& transaction();
  // Ends the statement's transaction as autocommit says: committed when no
  // explicit transaction is open, or kept.
  void end_statement(bool failed);
  // DDL commits what came before it.
  void implicit_commit();
  void rollback();
  // SERIALIZABLE is refused until read marks exist.
  void check_isolation() const;
  // A statement that reads takes its snapshot as the isolation level says.
  void begin_reading(txn::Transaction& t);
  // The database a [schema.]name refers to: the one named, else the current
  // one (1046 when there is none).
  std::string database_of(const std::vector<std::string>& name) const;
  // MySQL's checks on a database name: 1059, 1102, and 1044 for
  // information_schema.
  void check_database_name(const std::string& name) const;
  // character_set_database and collation_database: the current database's
  // defaults, else the server's.
  void follow_database();
  // The handshake's collation, if a session can speak it, as SET NAMES.
  void take_client_collation();
  // The collation results are reported in: character_set_results' default,
  // or the one SET NAMES ... COLLATE named; nothing when it is NULL.
  std::optional<uint16_t> results_collation() const;

  struct UserVariable {
    Value value;
    ResultType type;
  };

  const VariableRegistry& _registry;
  catalog::Catalog& _catalog;
  txn::Store& _rows;
  // The transaction under way, if any: an explicit one, or a statement's.
  std::unique_ptr<txn::Transaction> _transaction;
  // LAST_INSERT_ID(): the first value the last INSERT that made one made.
  uint64_t _last_insert_id = 0;
  // The transaction id a lock wait is registered for, if one is.
  std::optional<uint64_t> _waiting;
  SessionInfo _info;
  SessionVariables _variables;
  std::map<std::string, UserVariable> _user;
  std::optional<std::string> _database;
  // The collation SET NAMES ... COLLATE named, which results are reported in
  // while character_set_results is its character set; empty otherwise.
  std::string _results_collation;
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
