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

#include "dannan/contract/time_source.h"
#include "kvasir/catalog/catalog.h"
#include "kvasir/exec/expression.h"
#include "kvasir/exec/information_schema.h"
#include "kvasir/exec/variables.h"
#include "kvasir/protocol/statements.h"
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
// as READ COMMITTED; SERIALIZABLE as REPEATABLE READ with every plain
// SELECT in a transaction read FOR SHARE, its rows and key range locked. Not
// thread-safe: one connection's strand owns it (the catalog and store are
// safe from any thread).
namespace kvasir::exec {

// A FROM clause of joined tables (session.cpp).
struct JoinFrom;

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

// A prepared statement as COM_STMT_PREPARE answers it: its id, and each
// parameter's definition (named "?") and each result column's, typed before
// any value is bound, as the client is told them.
struct Prepared {
  uint32_t id = 0;
  std::vector<Column> params;
  std::vector<Column> columns;
};

// The most prepared statements a session holds (MySQL's
// max_prepared_stmt_count default, which MySQL counts across the server).
inline constexpr size_t max_prepared_statements = 16382;

struct SessionInfo {
  std::string user;
  std::string host;
  uint32_t connection_id = 0;
  // The collation the client asked for in its handshake; 0 for none.
  uint16_t client_collation = 0;
  // The wall clock NOW() and its kin read (Dannan's); none for the system's.
  // Tests hand in a stepped one.
  const dannan::contract::WallClock* clock = nullptr;
};

class Session : private VariableSource {
 public:
  // `catalog` and `rows` are the node's, and outlive the session.
  Session(const VariableRegistry& registry, catalog::Catalog& catalog, txn::Store& rows, SessionInfo info);
  // A session that ends rolls back its transaction, freeing its locks.
  ~Session();
  Session(Session&&) = default;
  Session& operator=(Session&&) = delete;

  // Prepares one statement, parsed with placeholders: refuses what MySQL
  // refuses at prepare (a table that is not there), and types its
  // parameters and its result without running it. Throws SqlError: 1461
  // past max_prepared_statements, and the statement's own refusals, which
  // are its diagnostics as a statement's are.
  Prepared prepare(sql::Statement statement);
  // A prepared statement's parameters' types as prepared. Throws SqlError
  // 1243, naming `command`, for an id not prepared.
  const std::vector<ResultType>& parameter_types(uint32_t id, const std::string& command) const;
  // The values a COM_STMT_EXECUTE bound, as the statement reads them
  // (observed): an integer LONGLONG, a FLOAT or DOUBLE DOUBLE, a DECIMAL
  // NEWDECIMAL, anything else a string in the connection's collation, NULL
  // its prepared type. Throws SqlError 1235 for a date or time value.
  std::vector<Parameter> bind(uint32_t id, const std::vector<protocol::BoundValue>& values) const;
  // Runs a prepared statement with its parameters, as execute() runs one.
  // Throws SqlError 1243 for an id not prepared.
  Outcome execute_prepared(uint32_t id, const std::vector<Parameter>& parameters);
  // Forgets a prepared statement; nothing for one not prepared.
  void close_prepared(uint32_t id);

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
  // A SELECT over a join of tables.
  Outcome select_joined(const sql::Select& select);
  // A SELECT with GROUP BY, HAVING or an aggregate, over `table`, or over
  // `join`'s tables (neither for a SELECT without FROM: one row).
  Outcome select_groups(const sql::Select& select, const catalog::Table* table, const std::string& database, JoinFrom* join = nullptr);
  Outcome insert(const sql::Statement& statement);
  // UPDATE and DELETE.
  Outcome change_rows(const sql::Statement& statement);
  // Multiple-table UPDATE and DELETE.
  Outcome change_joined(const sql::Statement& statement);
  // A SET's value stored in `row`'s column `index` (DEFAULT, NULL, the
  // value as the column stores it, AUTO_INCREMENT noted); `number` is the
  // row's, for warnings.
  void assign(const catalog::Table& table, size_t index, const sql::Expr& value, txn::Row& row, Evaluator& evaluator, uint64_t number);
  // The transaction a statement runs in: the open one, else a new one, which
  // autocommit commits when the statement ends.
  txn::Transaction& transaction();
  // Ends the statement's transaction as autocommit says: committed when no
  // explicit transaction is open, or kept.
  void end_statement(bool failed);
  // DDL commits what came before it.
  void implicit_commit();
  void rollback();
  // A statement that reads takes its snapshot as the isolation level says.
  void begin_reading(txn::Transaction& t);
  // INSERT ... SELECT's source is being read: inside the INSERT's statement,
  // which alone ends it, and as FOR SHARE unless under READ COMMITTED.
  bool _insert_source = false;
  // The statement's start, microseconds since the Unix epoch, from the
  // session's wall clock.
  int64_t _statement_us = 0;
  const dannan::contract::WallClock& wall_clock() const;
  // The statement's time as a DATETIME or TIMESTAMP column holds it: in the
  // session's zone, cut to the column's fraction digits.
  std::string clock_text(const catalog::Column& column) const;
  // The session's time_zone as seconds east of UTC.
  int32_t zone_seconds() const;
  // A row an UPDATE changed: its ON UPDATE columns it did not `set` take the
  // statement's time.
  void stamp(const catalog::Table& table, const txn::Row& before, txn::Row& after, const std::vector<bool>& set) const;
  // The SELECT's locking clause for the table it reads, by the name the
  // statement gives it: nothing for none; 3568 for an OF naming another
  // table, 3569 for two clauses over it (observed on MySQL 8.4.13).
  const sql::Select::Locking* locking_clause(const sql::Select& select, const std::string& qualifier) const;
  // The same for each of a join's tables, by their names.
  std::vector<const sql::Select::Locking*> locking_clauses(const sql::Select& select, const std::vector<std::string>& qualifiers) const;
  // A join's tables read: a table under a locking clause as a locking read
  // reads it (the latest rows, less those SKIP LOCKED passes over), the
  // others from the snapshot. True when any table is locked.
  bool read_join(JoinFrom& from, const std::vector<const sql::Select::Locking*>& clauses, txn::Transaction& t);
  // The rows each locked table gave `kept` joined rows locked, and its key
  // range (from WHERE) as a locking read locks it; a row held elsewhere
  // makes the statement wait, or with NOWAIT is 3572.
  void lock_join(JoinFrom& from, const std::vector<const sql::Select::Locking*>& clauses, txn::Transaction& t,
                 const std::vector<const std::vector<const txn::Row*>*>& kept, const sql::Expr* where);
  // A locking read's rows (SELECT ... FOR UPDATE / FOR SHARE): the rows as
  // UPDATE reads them, the latest committed and the transaction's own, less
  // those SKIP LOCKED passes over; the rows `passes` keeps locked in the
  // clause's mode, and the table's keys locked from others' inserts as
  // InnoDB's next-key locks keep them, but under READ COMMITTED or SKIP
  // LOCKED. A row held elsewhere makes the statement wait (LockWaitNeeded),
  // or, with NOWAIT, is 3572.
  std::vector<txn::Row> locked_rows(const sql::Select::Locking& clause, txn::Transaction& t, const catalog::Table& table, const txn::Store::KeyRange& range,
                                    const std::function<bool(const txn::Row&)>& passes);
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
  // 1044 for anything that would change information_schema.
  void refuse_information_schema(const std::string& database) const;
  // LIMIT's and OFFSET's count: a literal, or a bound parameter (1210 for
  // one that is not a count).
  uint64_t limit_of(const sql::Expr& e) const;
  // The table a [schema.]name names: 1046, 1049 or 1146 when there is none.
  std::optional<catalog::Table> named_table(const std::vector<std::string>& name) const;
  // A statement's parameters' types as MySQL infers them at prepare.
  std::vector<ResultType> infer_parameters(const sql::Statement& statement) const;
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
  // The prepared statements, by id: each statement, and its parameters'
  // types as prepared.
  struct PreparedStatement {
    sql::Statement statement;
    std::vector<ResultType> params;
  };
  std::map<uint32_t, PreparedStatement> _prepared;
  uint32_t _next_prepared = 1;
  // While a prepared statement runs: its parameters.
  const std::vector<Parameter>* _parameters = nullptr;
  // While a statement is prepared: SELECT types its result and stops before
  // reading any row.
  bool _describing = false;
  // While a SELECT reads an information_schema view: the view.
  const View* _view = nullptr;
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
