//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/session.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <utility>

#include "kvasir/exec/collation.h"
#include "kvasir/exec/ddl.h"
#include "kvasir/exec/rows.h"
#include "kvasir/exec/store.h"
#include "kvasir/protocol/messages.h"
#include "kvasir/protocol/types.h"
#include "kvasir/sql/keywords.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

bool has_mode(const std::string& modes, std::string_view name) {
  size_t start = 0;
  while (start <= modes.size()) {
    const auto comma = modes.find(',', start);
    const auto part = std::string_view(modes).substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (sql::same_word(part, name)) return true;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}

bool truth(const Value& v) {
  if (is_null(v)) return false;
  if (const auto* i = std::get_if<int64_t>(&v)) return *i != 0;
  if (const auto* u = std::get_if<uint64_t>(&v)) return *u != 0;
  if (const auto* d = std::get_if<Decimal>(&v)) return !d->is_zero();
  if (const auto* r = std::get_if<double>(&v)) return *r != 0;
  const auto& s = std::get<std::string>(v);
  return std::strtod(s.c_str(), nullptr) != 0;
}

// The type a user variable takes from the value assigned to it (observed).
ResultType user_variable_type(const ResultType& t) {
  ResultType out;
  out.decimals = protocol::not_fixed_decimals;
  if (t.field_type == f::longlong) {
    out.field_type = f::longlong;
    out.collation = protocol::collation::binary;
    out.decimals = 0;
    out.precision = t.is_unsigned() ? 21 : 20;
    out.length = 21;
    out.flags = static_cast<uint16_t>(c::binary | c::num | (t.is_unsigned() ? c::unsigned_ : 0));
  } else if (t.field_type == f::newdecimal) {
    out.field_type = f::newdecimal;
    out.collation = protocol::collation::binary;
    out.precision = decimal_max_precision;
    out.decimals = decimal_max_scale;
    out.length = 67;
    out.own_scale = true;
    out.flags = c::binary | c::num;
  } else if (t.field_type == f::double_) {
    out.field_type = f::double_;
    out.collation = protocol::collation::binary;
    out.length = 23;
    out.precision = 22;
    out.flags = c::binary | c::num;
  } else if (t.collation != protocol::collation::binary && t.field_type != f::null) {
    out.field_type = f::long_blob;
    out.collation = t.collation;
    out.length = 268435440;
  } else {
    out.field_type = f::medium_blob;
    out.collation = protocol::collation::binary;
    out.length = 16777215;
    out.flags = c::binary;
  }
  return out;
}

uint64_t limit_of(const sql::Expr& e) {
  if (e.kind != sql::ExprKind::integer) throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'placeholders'");
  uint64_t v = 0;
  const auto r = std::from_chars(e.value.data(), e.value.data() + e.value.size(), v);
  if (r.ec != std::errc()) return UINT64_MAX;
  return v;
}

}  // namespace

Session::Session(const VariableRegistry& registry, catalog::Catalog& catalog, txn::Store& rows, SessionInfo info)
    : _registry(registry), _catalog(catalog), _rows(rows), _info(std::move(info)), _variables(registry) {}

Session::~Session() {
  if (_waiting) _rows.stop_waiting(*_waiting);
  if (_transaction) _rows.rollback(*_transaction);
}

bool Session::wait_for_lock(const LockWaitNeeded& wait, std::function<void()> wake) {
  switch (_rows.wait(wait.waiter(), wait.holder(), wake)) {
    case txn::Store::Wait::waiting:
      _waiting = wait.waiter();
      return true;
    case txn::Store::Wait::free:
      // Let go already: run it again at once (`wake` only posts).
      wake();
      return true;
    case txn::Store::Wait::deadlock:
      break;
  }
  // The deadlock's victim: its whole transaction goes (InnoDB's choice when
  // its own wait closes the cycle).
  rollback();
  record_error(er_lock_deadlock, "Deadlock found when trying to get lock; try restarting transaction");
  return false;
}

void Session::give_up_waiting(const LockWaitNeeded& wait) {
  _rows.stop_waiting(wait.waiter());
  _waiting.reset();
  record_error(er_lock_wait_timeout, "Lock wait timeout exceeded; try restarting transaction");
}

uint64_t Session::lock_wait_timeout() const { return _variables.number("innodb_lock_wait_timeout"); }

void Session::reset() {
  rollback();
  _variables.reset();
  _user.clear();
  _database.reset();
  _read_only_transaction = false;
  _warnings.clear();
  _last_insert_id = 0;
}

txn::Transaction& Session::transaction() {
  if (!_transaction) {
    _transaction = std::make_unique<txn::Transaction>(_rows.begin());
    // Without autocommit the first statement opens the transaction.
    if (!_variables.number("autocommit")) _in_transaction = true;
  }
  return *_transaction;
}

void Session::end_statement(bool failed) {
  if (_in_transaction || !_transaction) return;
  if (failed) {
    _rows.rollback(*_transaction);
  } else {
    _rows.commit(*_transaction);
  }
  _transaction.reset();
}

void Session::implicit_commit() {
  if (_in_transaction || _transaction) commit();
}

void Session::rollback() {
  if (_transaction) _rows.rollback(*_transaction);
  _transaction.reset();
  _in_transaction = false;
}

EvalSettings Session::eval_settings() const {
  EvalSettings e;
  e.variables = this;
  e.div_precision_increment = static_cast<int>(_variables.number("div_precision_increment"));
  const auto* collation = find_collation(_variables.text("collation_connection"));
  e.result_collation = collation ? collation->id : protocol::collation::utf8mb4_0900_ai_ci;
  e.result_max_bytes = collation && collation->charset == "binary" ? 1 : 4;
  e.result_collation_binary = collation && collation->name.size() > 4 && collation->name.compare(collation->name.size() - 4, 4, "_bin") == 0;
  return e;
}

// @@warning_count and @@error_count count the diagnostics of the statement
// before; every other variable is the registry's.
static bool counts_diagnostics(const std::string& scope, const std::string& name) {
  return (scope.empty() || sql::same_word(scope, "SESSION")) && (sql::same_word(name, "warning_count") || sql::same_word(name, "error_count"));
}

ResultType Session::system_type(const std::string& scope, const std::string& name) const {
  if (counts_diagnostics(scope, name)) {
    ResultType t;
    t.field_type = f::longlong;
    t.collation = protocol::collation::binary;
    t.length = 21;
    t.precision = 21;
    t.flags = c::unsigned_ | c::binary | c::num;
    return t;
  }
  const auto e = eval_settings();
  return _variables.type_of(scope, name, e.result_collation, e.result_max_bytes);
}

Value Session::system_value(const std::string& scope, const std::string& name) const {
  if (counts_diagnostics(scope, name)) {
    uint64_t n = 0;
    for (const auto& w : _previous)
      if (sql::same_word(name, "warning_count") || w.level == "Error") ++n;
    return n;
  }
  return _variables.get(scope, name);
}

ResultType Session::user_type(const std::string& name) const {
  const auto it = _user.find(lower(name));
  if (it != _user.end()) return it->second.type;
  // Never set: NULL, a binary string (observed).
  ResultType t;
  t.field_type = f::var_string;
  t.collation = protocol::collation::binary;
  t.length = 65532;
  t.decimals = protocol::not_fixed_decimals;
  t.flags = c::binary;
  return t;
}

Value Session::user_value(const std::string& name) const {
  const auto it = _user.find(lower(name));
  return it == _user.end() ? Value{} : it->second.value;
}

uint16_t Session::status_flags() const {
  uint16_t flags = 0;
  if (_in_transaction) flags |= protocol::status::in_transaction;
  if (_variables.number("autocommit")) flags |= protocol::status::autocommit;
  return flags;
}

sql::ParserOptions Session::parser_options() const {
  const auto modes = _variables.text("sql_mode");
  sql::ParserOptions o;
  o.lexer.ansi_quotes = has_mode(modes, "ANSI_QUOTES");
  o.lexer.no_backslash_escapes = has_mode(modes, "NO_BACKSLASH_ESCAPES");
  o.pipes_as_concat = has_mode(modes, "PIPES_AS_CONCAT");
  return o;
}

void Session::commit() {
  if (_transaction) _rows.commit(*_transaction);
  _transaction.reset();
  _in_transaction = false;
}

void Session::record_error(uint16_t code, const std::string& message) {
  _previous = std::move(_warnings);
  _warnings = {{"Error", code, message}};
}

namespace {

ResultType text_type(const EvalSettings& e, uint32_t characters, bool not_null) {
  ResultType t;
  t.field_type = f::var_string;
  t.collation = e.result_collation;
  t.length = characters * e.result_max_bytes;
  t.decimals = protocol::not_fixed_decimals;
  t.flags = static_cast<uint16_t>((not_null ? c::not_null : 0) | (not_null && e.result_collation_binary ? c::binary : 0));
  return t;
}

ResultType bigint_type(bool is_unsigned, bool not_null) {
  ResultType t;
  t.field_type = f::longlong;
  t.collation = protocol::collation::binary;
  t.length = 21;
  t.precision = is_unsigned ? 21 : 20;
  t.flags = static_cast<uint16_t>(c::binary | c::num | (is_unsigned ? c::unsigned_ : 0) | (not_null ? c::not_null : 0));
  return t;
}

size_t characters(const std::string& s) {
  size_t n = 0;
  for (const char ch : s)
    if ((static_cast<uint8_t>(ch) & 0xc0) != 0x80) ++n;
  return n;
}

}  // namespace

// The functions of the session, with the types MySQL 8.4 gives them (observed).
// An account has no host part in Kvasir, so CURRENT_USER() is user@%.
bool Session::session_function(const std::string& name, ResultType& type, Value& value) const {
  const auto e = eval_settings();
  if (name == "DATABASE" || name == "SCHEMA") {
    type = text_type(e, 64, false);
    value = _database ? Value{*_database} : Value{};
  } else if (name == "VERSION") {
    const auto version = _variables.text("version");
    type = text_type(e, static_cast<uint32_t>(characters(version)), true);
    value = version;
  } else if (name == "USER" || name == "SESSION_USER" || name == "SYSTEM_USER") {
    type = text_type(e, 288, false);
    value = _info.user + "@" + _info.host;
  } else if (name == "CURRENT_USER") {
    type = text_type(e, 288, false);
    value = _info.user + "@%";
  } else if (name == "CONNECTION_ID") {
    type = bigint_type(true, true);
    value = uint64_t{_info.connection_id};
  } else if (name == "LAST_INSERT_ID") {
    type = bigint_type(true, true);
    value = _last_insert_id;
  } else if (name == "ROW_COUNT") {
    type = bigint_type(false, true);
    value = _row_count;
  } else if (name == "FOUND_ROWS") {
    type = bigint_type(false, true);
    value = static_cast<int64_t>(_found_rows);
  } else {
    return false;
  }
  return true;
}

// SHOW WARNINGS and SHOW ERRORS: the diagnostics of the statement before,
// left in place.
Outcome Session::show_warnings(const sql::Statement& statement) {
  const auto e = eval_settings();
  ResultSet result;
  std::vector<const Warning*> listed;
  for (const auto& w : _previous)
    if (!statement.errors_only || w.level == "Error") listed.push_back(&w);
  if (statement.count_only) {
    result.columns.push_back({statement.errors_only ? "@@session.error_count" : "@@session.warning_count", bigint_type(true, false)});
    result.rows.push_back({std::to_string(listed.size())});
  } else {
    ResultType code;
    code.field_type = f::long_;
    code.collation = protocol::collation::binary;
    code.length = 5;
    code.flags = c::not_null | c::unsigned_ | c::binary | c::num;
    result.columns.push_back({"Level", text_type(e, 7, true)});
    result.columns.push_back({"Code", code});
    result.columns.push_back({"Message", text_type(e, 512, true)});
    const uint64_t offset = statement.select.offset ? limit_of(*statement.select.offset) : 0;
    const uint64_t limit = statement.select.limit ? limit_of(*statement.select.limit) : UINT64_MAX;
    for (size_t i = 0; i < listed.size(); ++i) {
      if (i < offset || i - offset >= limit) continue;
      result.rows.push_back({listed[i]->level, std::to_string(listed[i]->code), listed[i]->message});
    }
  }
  _warnings = _previous;
  Outcome out;
  out.result = std::move(result);
  return out;
}

Outcome Session::execute(const sql::Statement& statement) {
  // The statement before's diagnostics stay readable (SHOW WARNINGS,
  // @@warning_count) while this one runs; then this one's replace them, an
  // error among them.
  _waiting.reset();
  _previous = std::move(_warnings);
  _warnings.clear();
  try {
    auto outcome = run(statement);
    if (outcome.result) {
      _row_count = -1;
      _found_rows = outcome.result->rows.size();
    } else {
      _row_count = static_cast<int64_t>(outcome.affected_rows);
    }
    return outcome;
  } catch (const SqlError& e) {
    _warnings.push_back({"Error", e.code(), e.what()});
    throw;
  } catch (const LockWaitNeeded&) {
    // Not run yet, as far as anyone can tell: the diagnostics stay the
    // statement before's.
    _warnings = std::move(_previous);
    throw;
  }
}

Outcome Session::run(const sql::Statement& statement) {
  using Kind = sql::Statement::Kind;
  if (!statement.unsupported.empty())
    throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support '" + statement.unsupported + "'");
  switch (statement.kind) {
    case Kind::select:
      return select(statement.select);
    case Kind::show_warnings:
      return show_warnings(statement);
    case Kind::set:
      set(statement.assignments);
      return {};
    case Kind::use:
      if (!_catalog.database(statement.database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + statement.database + "'");
      _database = statement.database;
      return {};
    case Kind::create_database:
      implicit_commit();
      return create_database(statement);
    case Kind::drop_database:
      implicit_commit();
      return drop_database(statement);
    case Kind::show_databases:
      return show_databases(statement);
    case Kind::begin:
      implicit_commit();
      _in_transaction = true;
      _read_only_transaction = statement.read_only;
      return {};
    case Kind::commit:
      commit();
      return {};
    case Kind::rollback:
      rollback();
      return {};
    case Kind::create_table:
      implicit_commit();
      return create_table(statement);
    case Kind::drop_table:
      implicit_commit();
      return drop_table(statement);
    case Kind::show_tables:
      return show_tables(statement);
    case Kind::insert:
      return insert(statement);
    case Kind::update:
    case Kind::delete_:
      return change_rows(statement);
  }
  return {};
}

namespace {

// A 1054 raised while typing a clause, named for that clause.
template <typename F>
void in_clause(const char* clause, F&& f) {
  try {
    f();
  } catch (const SqlError& e) {
    if (e.code() != er_bad_field_error) throw;
    std::string message = e.what();
    const std::string field = "'field list'";
    if (const auto at = message.rfind(field); at != std::string::npos) message.replace(at, field.size(), std::string("'") + clause + "'");
    throw SqlError(e.code(), e.sqlstate(), message);
  }
}

// One table's columns, as a statement over it names them: column,
// table.column (by its alias when it has one) or schema.table.column, read
// from the row at hand.
class TableColumns : public ColumnSource {
 public:
  TableColumns(const catalog::Table& table, std::string database, std::string alias, const EvalSettings& eval)
      : _table(table), _database(std::move(database)), _alias(std::move(alias)), _eval(eval) {}

  std::optional<size_t> index(const std::vector<std::string>& path) const {
    if (path.size() == 2 && path[0] != qualifier()) return std::nullopt;
    if (path.size() == 3 && (!_alias.empty() || path[0] != _database || path[1] != _table.name)) return std::nullopt;
    return _table.column(path.back());
  }

  std::optional<Found> find_column(const std::vector<std::string>& path) const override {
    const auto i = index(path);
    if (!i) return std::nullopt;
    return Found{column_result_type(_table, *i, _eval), column_collation_id(_table.columns[*i])};
  }

  Value column_value(const std::vector<std::string>& path) const override {
    const auto i = index(path);
    if (!i || !_row || !(*_row)[*i]) return {};
    return value_of(_table.columns[*i].type, *(*_row)[*i]);
  }

  void set_row(const txn::Row* row) { _row = row; }

  // A column's value as a client is sent it.
  std::optional<std::string> text(size_t i) const {
    if (!_row || !(*_row)[i]) return std::nullopt;
    return display_text(_table.columns[i].type, *(*_row)[i], column_result_type(_table, i, _eval).length);
  }

  // The name the table goes by in the statement.
  const std::string& qualifier() const { return _alias.empty() ? _table.name : _alias; }

  Column column(size_t i, std::string name) const {
    Column out(std::move(name), column_result_type(_table, i, _eval));
    out.schema = _database;
    out.table = qualifier();
    out.original_table = _table.name;
    out.original_name = _table.columns[i].name;
    return out;
  }

 private:
  const catalog::Table& _table;
  std::string _database;
  std::string _alias;
  EvalSettings _eval;
  const txn::Row* _row = nullptr;
};

// Whether an expression names a column anywhere in it.
bool mentions_column(const sql::Expr& e) {
  if (e.kind == sql::ExprKind::column) return true;
  for (const auto& a : e.args)
    if (a && mentions_column(*a)) return true;
  return (e.operand && mentions_column(*e.operand)) || (e.otherwise && mentions_column(*e.otherwise));
}

// Text a column's character set cannot hold, as error 1366 shows it: from
// the first byte past ASCII, each such byte as \xHH, at most six.
std::string escaped_bytes(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && static_cast<uint8_t>(s[i]) < 0x80) ++i;
  std::string out;
  static const char* const hex = "0123456789ABCDEF";
  for (size_t n = 0; i < s.size() && n < 6; ++i, ++n) {
    const auto b = static_cast<uint8_t>(s[i]);
    if (b < 0x80) {
      out.push_back(static_cast<char>(b));
    } else {
      out += "\\x";
      out.push_back(hex[b >> 4]);
      out.push_back(hex[b & 0xf]);
    }
  }
  return out;
}

// A value as a column stores it, or the error MySQL gives a row for it
// (strict mode).
std::string stored_text(const catalog::Column& column, const Value& value, const ResultType& from, size_t row_number, std::vector<Warning>& warnings) {
  const std::string at_row = " at row " + std::to_string(row_number);
  const auto stored = store(column.type, value, from);
  const auto value_text = [&]() { return to_text(value, from).value_or("NULL"); };
  switch (stored.result) {
    case Stored::Result::truncated:
      warnings.push_back({"Note", 1265, "Data truncated for column '" + column.name + "'" + at_row});
      [[fallthrough]];
    case Stored::Result::stored:
      return stored.text;
    case Stored::Result::out_of_range:
      throw SqlError(1264, "22003", "Out of range value for column '" + column.name + "'" + at_row);
    case Stored::Result::too_long:
      throw SqlError(1406, "22001", "Data too long for column '" + column.name + "'" + at_row);
    case Stored::Result::garbage:
      if (column.type.kind != catalog::TypeKind::decimal) throw SqlError(1265, "01000", "Data truncated for column '" + column.name + "'" + at_row);
      [[fallthrough]];
    case Stored::Result::invalid:
      break;
  }
  if (catalog::is_text(column.type.kind))
    throw SqlError(1366, "HY000", "Incorrect string value: '" + escaped_bytes(value_text()) + "' for column '" + column.name + "'" + at_row);
  throw SqlError(1366, "HY000",
                 std::string("Incorrect ") + (column.type.kind == catalog::TypeKind::decimal ? "decimal" : "integer") + " value: '" + value_text() +
                     "' for column '" + column.name + "'" + at_row);
}

// The row's unique key values, each with its key's name, for the store.
std::vector<std::string> unique_names(const catalog::Table& table, std::optional<size_t> primary, const txn::Row& row) {
  std::vector<std::string> out;
  for (size_t k = 0; k < table.keys.size(); ++k) {
    const auto& key = table.keys[k];
    if (key.kind == catalog::Key::Kind::index || primary == k || key_has_null(key, row)) continue;
    out.push_back(key.name + std::string(1, '\0') + encode_key(table, key, row));
  }
  return out;
}

// Error 1062 for the key the store says another row holds ("" for the
// primary key).
[[noreturn]] void duplicate_entry(const catalog::Table& table, std::optional<size_t> primary, const std::string& duplicate, const txn::Row& row) {
  const auto name = duplicate.substr(0, duplicate.find('\0'));
  const auto& key =
      duplicate.empty() ? table.keys[*primary] : *std::find_if(table.keys.begin(), table.keys.end(), [&](const catalog::Key& k) { return k.name == name; });
  throw SqlError(1062, "23000", "Duplicate entry '" + key_text(key, row) + "' for key '" + table.name + "." + key.name + "'");
}

// The collation an ORDER BY value sorts under: a column's own, else the
// results'.
uint16_t order_collation(const sql::Expr& e, const ResultType& type, const TableColumns& columns, uint16_t results) {
  if (e.kind == sql::ExprKind::column)
    if (const auto found = columns.find_column(e.path)) return found->collation;
  return type.collation == protocol::collation::binary ? protocol::collation::binary : results;
}

}  // namespace

void Session::check_isolation() const {
  if (sql::same_word(_variables.text("transaction_isolation"), "SERIALIZABLE"))
    throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'SERIALIZABLE'");
}

void Session::begin_reading(txn::Transaction& t) {
  // READ COMMITTED (and READ UNCOMMITTED, served as it) reads afresh each
  // statement; REPEATABLE READ keeps the snapshot of its first read.
  const auto level = _variables.text("transaction_isolation");
  _rows.take_snapshot(t, sql::same_word(level, "READ-COMMITTED") || sql::same_word(level, "READ-UNCOMMITTED"));
}

std::string Session::database_of(const std::vector<std::string>& name) const {
  if (name.size() == 2) return name[0];
  if (!_database) throw SqlError(er_no_db_error, "3D000", "No database selected");
  return *_database;
}

Outcome Session::select_from_table(const sql::Select& select) {
  if (select.from.size() > 1) throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'joins'");
  const auto& from = select.from.front();
  const auto database = database_of(from.name);
  const auto table = _catalog.table({database, from.name.back()});
  if (!table) {
    if (!_catalog.database(database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
    throw SqlError(1146, "42S02", "Table '" + database + "." + from.name.back() + "' doesn't exist");
  }
  check_isolation();
  auto settings = eval_settings();
  TableColumns columns(*table, database, from.alias, settings);
  settings.columns = &columns;
  Evaluator evaluator(settings);
  ResultSet result;
  // What each result column is made of: a table column, or an expression.
  struct Output {
    std::optional<size_t> column;
    const sql::Expr* expr = nullptr;
    ResultType type;
  };
  std::vector<Output> outputs;
  for (const auto& item : select.items) {
    if (item.star) {
      // *, table.* or schema.table.*
      const auto& p = item.star_path;
      const bool ours =
          p.empty() || (p.size() == 1 && p[0] == columns.qualifier()) || (p.size() == 2 && from.alias.empty() && p[0] == database && p[1] == table->name);
      if (!ours) throw SqlError(1051, "42S02", "Unknown table '" + p.back() + "'");
      for (size_t i = 0; i < table->columns.size(); ++i) {
        result.columns.push_back(columns.column(i, table->columns[i].name));
        outputs.push_back({i, nullptr, result.columns.back().type});
      }
      continue;
    }
    const auto type = evaluator.type(*item.expr);
    if (item.expr->kind == sql::ExprKind::column) {
      // A column keeps the name it was written with, without its qualifier.
      const auto i = *columns.index(item.expr->path);
      result.columns.push_back(columns.column(i, item.has_alias ? item.alias : item.expr->path.back()));
      outputs.push_back({i, item.expr.get(), type});
    } else {
      result.columns.push_back({column_name(item), type});
      outputs.push_back({std::nullopt, item.expr.get(), type});
    }
  }
  if (select.where) in_clause("where clause", [&] { evaluator.type(*select.where); });
  // ORDER BY: a select item by its position or its alias, else an
  // expression over the row.
  struct Sort {
    const sql::Expr* expr = nullptr;
    ResultType type;
    uint16_t collation = 0;
    bool descending = false;
  };
  std::vector<Sort> sorts;
  for (const auto& order : select.order) {
    const sql::Expr* e = order.expr.get();
    if (e->kind == sql::ExprKind::integer) {
      const uint64_t position = limit_of(*e);
      if (position < 1 || position > select.items.size() || select.items[position - 1].star)
        throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + e->value + "' in 'order clause'");
      e = select.items[position - 1].expr.get();
    } else if (e->kind == sql::ExprKind::column && e->path.size() == 1) {
      for (const auto& item : select.items)
        if (item.has_alias && sql::same_word(item.alias, e->path[0])) e = item.expr.get();
    }
    Sort sort;
    sort.expr = e;
    sort.descending = order.descending;
    in_clause("order clause", [&] { sort.type = evaluator.type(*e); });
    sort.collation = order_collation(*e, sort.type, columns, settings.result_collation);
    sorts.push_back(sort);
  }
  const uint64_t limit = select.limit ? limit_of(*select.limit) : _variables.number("sql_select_limit");
  const uint64_t offset = select.offset ? limit_of(*select.offset) : 0;

  auto& t = transaction();
  try {
    begin_reading(t);
    const auto rows = _rows.rows(t, table->id);
    struct Chosen {
      const txn::Row* row;
      std::vector<Value> keys;
    };
    std::vector<Chosen> chosen;
    for (const auto& row : rows) {
      columns.set_row(&row);
      if (select.where && !truth(evaluator.value(*select.where))) continue;
      Chosen c{&row, {}};
      for (const auto& sort : sorts) c.keys.push_back(evaluator.value(*sort.expr));
      chosen.push_back(std::move(c));
    }
    if (!sorts.empty()) {
      std::stable_sort(chosen.begin(), chosen.end(), [&](const Chosen& a, const Chosen& b) {
        for (size_t i = 0; i < sorts.size(); ++i) {
          const int order = compare_for_order(a.keys[i], b.keys[i], sorts[i].collation);
          if (order != 0) return sorts[i].descending ? order > 0 : order < 0;
        }
        return false;
      });
    }
    for (size_t i = 0; i < chosen.size(); ++i) {
      if (i < offset || i - offset >= limit) continue;
      columns.set_row(chosen[i].row);
      std::vector<std::optional<std::string>> out;
      for (const auto& o : outputs) out.push_back(o.column ? columns.text(*o.column) : to_text(evaluator.value(*o.expr), o.type));
      result.rows.push_back(std::move(out));
    }
    columns.set_row(nullptr);
  } catch (...) {
    end_statement(true);
    throw;
  }
  end_statement(false);
  _warnings = evaluator.warnings();
  Outcome out;
  out.result = std::move(result);
  return out;
}

Outcome Session::insert(const sql::Statement& statement) {
  const auto& in = statement.insert;
  const auto database = database_of(in.table);
  const auto table = _catalog.table({database, in.table.back()});
  if (!table) {
    if (!_catalog.database(database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
    throw SqlError(1146, "42S02", "Table '" + database + "." + in.table.back() + "' doesn't exist");
  }
  check_isolation();

  // The columns given, by index.
  std::vector<size_t> given;
  if (in.has_columns) {
    for (const auto& name : in.columns) {
      const auto i = table->column(name);
      if (!i) throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + name + "' in 'field list'");
      if (std::find(given.begin(), given.end(), *i) != given.end()) throw SqlError(1110, "42000", "Column '" + name + "' specified twice");
      given.push_back(*i);
    }
  } else {
    for (size_t i = 0; i < table->columns.size(); ++i) given.push_back(i);
  }
  for (size_t r = 0; r < in.rows.size(); ++r) {
    // VALUES () gives every column its default.
    if (in.rows[r].size() != given.size() && !(!in.has_columns && in.rows[r].empty()))
      throw SqlError(1136, "21S01", "Column count doesn't match value count at row " + std::to_string(r + 1));
    for (const auto& e : in.rows[r])
      if (mentions_column(*e)) throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'columns in VALUES'");
  }

  auto& t = transaction();
  _rows.begin_statement(t);
  Outcome outcome;
  try {
    Evaluator evaluator(eval_settings());
    const auto primary = table->primary_key();
    // AUTO_INCREMENT: the statement's first need reserves a value for each
    // of its rows (observed: the values of a failed statement are lost).
    uint64_t next_auto = 0, reserved = 0, first_auto = 0;
    for (size_t r = 0; r < in.rows.size(); ++r) {
      txn::Row row(table->columns.size());
      std::vector<bool> set(table->columns.size(), false);
      std::optional<size_t> generate;
      const auto use_default = [&](size_t i) {
        const auto& column = table->columns[i];
        if (column.auto_increment) {
          generate = i;
        } else if (column.default_kind == catalog::Column::Default::value) {
          row[i] = column.default_value;
        } else if (column.default_kind == catalog::Column::Default::none && !column.nullable) {
          throw SqlError(1364, "HY000", "Field '" + column.name + "' doesn't have a default value");
        }
        set[i] = true;
      };
      for (size_t j = 0; j < in.rows[r].size(); ++j) {
        const size_t i = given[j];
        const auto& column = table->columns[i];
        const auto& e = *in.rows[r][j];
        if (e.kind == sql::ExprKind::default_value) {
          use_default(i);
          continue;
        }
        const auto from = evaluator.type(e);
        const auto value = evaluator.value(e);
        set[i] = true;
        if (is_null(value)) {
          if (column.auto_increment) {
            generate = i;
          } else if (!column.nullable) {
            throw SqlError(1048, "23000", "Column '" + column.name + "' cannot be null");
          }
          continue;
        }
        row[i] = stored_text(column, value, from, r + 1, _warnings);
        // 0 in an AUTO_INCREMENT column asks for the next value.
        if (column.auto_increment) {
          if (*row[i] == "0") {
            generate = i;
            row[i].reset();
          } else if ((*row[i])[0] != '-') {
            _rows.saw_auto(table->id, table->auto_increment, std::stoull(*row[i]));
          }
        }
      }
      for (size_t i = 0; i < table->columns.size(); ++i)
        if (!set[i]) use_default(i);
      if (generate) {
        if (reserved == 0) {
          reserved = in.rows.size() - r;
          next_auto = _rows.reserve_auto(table->id, table->auto_increment, reserved);
        }
        const uint64_t value = next_auto++;
        --reserved;
        if (first_auto == 0) first_auto = value;
        row[*generate] = std::to_string(value);
      }
      std::string key;
      if (primary) {
        key = encode_key(*table, table->keys[*primary], row);
      } else {
        key.assign(8, '\0');
        uint64_t id = _rows.next_row_id(table->id);
        for (int b = 7; b >= 0; --b, id >>= 8) key[static_cast<size_t>(b)] = static_cast<char>(id & 0xff);
      }
      std::string duplicate;
      uint64_t holder = 0;
      switch (_rows.insert(t, table->id, key, unique_names(*table, primary, row), row, &duplicate, &holder)) {
        case txn::Store::Inserted::inserted:
          ++outcome.affected_rows;
          break;
        case txn::Store::Inserted::locked:
          throw LockWaitNeeded(t.id(), holder);
        case txn::Store::Inserted::duplicate:
          duplicate_entry(*table, primary, duplicate, row);
      }
    }
    if (first_auto) {
      _last_insert_id = first_auto;
      outcome.last_insert_id = first_auto;
    }
  } catch (...) {
    _rows.undo_statement(t);
    end_statement(true);
    throw;
  }
  end_statement(false);
  if (in.rows.size() > 1) outcome.info = "Records: " + std::to_string(in.rows.size()) + "  Duplicates: 0  Warnings: " + std::to_string(_warnings.size());
  return outcome;
}

Outcome Session::change_rows(const sql::Statement& statement) {
  const bool deleting = statement.kind == sql::Statement::Kind::delete_;
  const auto& c = statement.change;
  const auto database = database_of(c.table.name);
  const auto table = _catalog.table({database, c.table.name.back()});
  if (!table) {
    if (!_catalog.database(database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
    throw SqlError(1146, "42S02", "Table '" + database + "." + c.table.name.back() + "' doesn't exist");
  }
  check_isolation();
  auto settings = eval_settings();
  TableColumns columns(*table, database, c.table.alias, settings);
  settings.columns = &columns;
  Evaluator evaluator(settings);
  // SET's columns, and its values typed (an unknown column is in the field
  // list).
  struct Set {
    size_t column;
    const sql::Expr* value;
  };
  std::vector<Set> sets;
  for (const auto& [path, value] : c.set) {
    const auto i = columns.index(path);
    if (!i) {
      std::string name;
      for (size_t k = 0; k < path.size(); ++k) name += (k ? "." : "") + path[k];
      throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + name + "' in 'field list'");
    }
    if (value->kind != sql::ExprKind::default_value) evaluator.type(*value);
    sets.push_back({*i, value.get()});
  }
  if (c.where) in_clause("where clause", [&] { evaluator.type(*c.where); });
  std::vector<std::pair<const sql::Expr*, uint16_t>> sorts;
  for (const auto& order : c.order) {
    ResultType type;
    in_clause("order clause", [&] { type = evaluator.type(*order.expr); });
    sorts.emplace_back(order.expr.get(), order_collation(*order.expr, type, columns, settings.result_collation));
  }
  const uint64_t limit = c.limit ? limit_of(*c.limit) : UINT64_MAX;

  auto& t = transaction();
  _rows.begin_statement(t);
  Outcome outcome;
  uint64_t matched = 0;
  try {
    // InnoDB's current read: the latest committed rows and this
    // transaction's own.
    auto rows = _rows.current(t, table->id);
    struct Chosen {
      size_t index;
      std::vector<Value> keys;
    };
    std::vector<Chosen> chosen;
    for (size_t i = 0; i < rows.size(); ++i) {
      columns.set_row(&rows[i].row);
      if (c.where && !truth(evaluator.value(*c.where))) continue;
      Chosen ch{i, {}};
      for (const auto& sort : sorts) ch.keys.push_back(evaluator.value(*sort.first));
      chosen.push_back(std::move(ch));
    }
    if (!sorts.empty()) {
      std::stable_sort(chosen.begin(), chosen.end(), [&](const Chosen& a, const Chosen& b) {
        for (size_t i = 0; i < sorts.size(); ++i) {
          const int order = compare_for_order(a.keys[i], b.keys[i], sorts[i].second);
          if (order != 0) return c.order[i].descending ? order > 0 : order < 0;
        }
        return false;
      });
    }
    if (chosen.size() > limit) chosen.resize(limit);
    const auto primary = table->primary_key();
    for (const auto& ch : chosen) {
      const auto& [key, old_row] = rows[ch.index];
      ++matched;
      uint64_t holder = 0;
      if (deleting) {
        if (!_rows.remove(t, table->id, key, &holder)) throw LockWaitNeeded(t.id(), holder);
        ++outcome.affected_rows;
        continue;
      }
      // Each value sees the columns set before it.
      txn::Row row = old_row;
      columns.set_row(&row);
      for (const auto& set : sets) {
        const auto& column = table->columns[set.column];
        if (set.value->kind == sql::ExprKind::default_value) {
          if (column.default_kind == catalog::Column::Default::value) {
            row[set.column] = column.default_value;
          } else if (column.default_kind == catalog::Column::Default::none && !column.nullable) {
            throw SqlError(1364, "HY000", "Field '" + column.name + "' doesn't have a default value");
          } else {
            row[set.column].reset();
          }
          continue;
        }
        const auto from = evaluator.type(*set.value);
        const auto value = evaluator.value(*set.value);
        if (is_null(value)) {
          if (!column.nullable) throw SqlError(1048, "23000", "Column '" + column.name + "' cannot be null");
          row[set.column].reset();
          continue;
        }
        row[set.column] = stored_text(column, value, from, matched, _warnings);
        if (column.auto_increment && (*row[set.column])[0] != '-') _rows.saw_auto(table->id, table->auto_increment, std::stoull(*row[set.column]));
      }
      columns.set_row(nullptr);
      if (row != old_row) ++outcome.affected_rows;
      const std::string new_key = primary ? encode_key(*table, table->keys[*primary], row) : key;
      std::string duplicate;
      switch (_rows.update(t, table->id, key, new_key, unique_names(*table, primary, row), row, &duplicate, &holder)) {
        case txn::Store::Inserted::inserted:
          break;
        case txn::Store::Inserted::locked:
          throw LockWaitNeeded(t.id(), holder);
        case txn::Store::Inserted::duplicate:
          duplicate_entry(*table, primary, duplicate, row);
      }
    }
  } catch (...) {
    columns.set_row(nullptr);
    _rows.undo_statement(t);
    end_statement(true);
    throw;
  }
  end_statement(false);
  _warnings.insert(_warnings.end(), evaluator.warnings().begin(), evaluator.warnings().end());
  if (!deleting)
    outcome.info =
        "Rows matched: " + std::to_string(matched) + "  Changed: " + std::to_string(outcome.affected_rows) + "  Warnings: " + std::to_string(_warnings.size());
  return outcome;
}

Outcome Session::select(const sql::Select& select) {
  if (!select.from.empty()) return select_from_table(select);
  Evaluator evaluator(eval_settings());
  ResultSet result;
  for (const auto& item : select.items) {
    if (item.star) throw SqlError(er_no_tables_used, "HY000", "No tables used");
    result.columns.push_back({column_name(item), evaluator.type(*item.expr)});
  }
  for (const auto& order : select.order) {
    try {
      evaluator.type(*order.expr);
    } catch (const SqlError& e) {
      if (e.code() != er_bad_field_error) throw;
      std::string message = e.what();
      const std::string field = "'field list'";
      if (const auto at = message.rfind(field); at != std::string::npos) message.replace(at, field.size(), "'order clause'");
      throw SqlError(e.code(), e.sqlstate(), message);
    }
  }
  const uint64_t limit = select.limit ? limit_of(*select.limit) : _variables.number("sql_select_limit");
  const uint64_t offset = select.offset ? limit_of(*select.offset) : 0;
  const bool passes = !select.where || truth(evaluator.value(*select.where));
  if (passes && offset == 0 && limit > 0) {
    std::vector<std::optional<std::string>> row;
    for (size_t i = 0; i < select.items.size(); ++i) row.push_back(to_text(evaluator.value(*select.items[i].expr), result.columns[i].type));
    result.rows.push_back(std::move(row));
  }
  _warnings = evaluator.warnings();
  Outcome out;
  out.result = std::move(result);
  return out;
}

void Session::set(const std::vector<sql::Assignment>& assignments) {
  using Target = sql::Assignment::Target;
  Evaluator evaluator(eval_settings());
  struct Pending {
    Value value;
    ResultType type;
    bool use_default = false;
  };
  // Every value is computed before any is assigned (observed: SET @a = 1,
  // @b = @a + 1 leaves @b NULL).
  std::vector<Pending> pending;
  for (const auto& a : assignments) {
    Pending p;
    if (a.target == Target::user_variable) {
      p.type = evaluator.type(*a.value);
      p.value = evaluator.value(*a.value);
    } else if (a.target == Target::system_variable) {
      const auto& e = *a.value;
      if (e.kind == sql::ExprKind::default_value) {
        p.use_default = true;
      } else if (e.kind == sql::ExprKind::keyword_value) {
        p.value = e.value;
      } else if (e.kind == sql::ExprKind::column && e.path.size() == 1) {
        // A bare word names the value: SET autocommit = OFF.
        p.value = e.path[0];
      } else {
        p.value = evaluator.value(e);
      }
    }
    pending.push_back(std::move(p));
  }
  _warnings = evaluator.warnings();

  const auto set_charset = [&](const std::string& charset, const std::string& collation_name) {
    const auto* collation = default_collation(charset);
    if (!collation_name.empty()) {
      collation = find_collation(collation_name);
      if (!collation) throw SqlError(1273, "HY000", "Unknown collation: '" + collation_name + "'");
      if (!sql::same_word(collation->charset, charset))
        throw SqlError(1253, "42000", "COLLATION '" + collation_name + "' is not valid for CHARACTER SET '" + charset + "'");
    }
    return collation;
  };

  for (size_t i = 0; i < assignments.size(); ++i) {
    const auto& a = assignments[i];
    auto& p = pending[i];
    switch (a.target) {
      case Target::user_variable:
        _user[lower(a.name)] = UserVariable{p.type.field_type == f::null ? Value{} : std::move(p.value), user_variable_type(p.type)};
        break;
      case Target::system_variable: {
        _variables.set(a.scope, a.name, p.value, p.use_default, _warnings);
        if (sql::same_word(a.name, "character_set_connection") && !p.use_default) {
          if (const auto* d = default_collation(_variables.text("character_set_connection")))
            _variables.set("", "collation_connection", d->name, false, _warnings);
        }
        if (sql::same_word(a.name, "collation_connection")) {
          if (const auto* col = find_collation(_variables.text("collation_connection")))
            _variables.set("", "character_set_connection", col->charset, false, _warnings);
        }
        if (sql::same_word(a.name, "autocommit") && _variables.number("autocommit") && _in_transaction) commit();
        break;
      }
      case Target::names: {
        const std::string charset = a.use_default ? _variables.text("character_set_server") : a.charset;
        _variables.set("", "character_set_client", charset, false, _warnings);
        const auto* collation = set_charset(lower(charset), a.collation);
        _variables.set("", "character_set_connection", charset, false, _warnings);
        _variables.set("", "character_set_results", charset, false, _warnings);
        _variables.set("", "collation_connection", collation->name, false, _warnings);
        break;
      }
      case Target::character_set: {
        const std::string charset = a.use_default ? _variables.text("character_set_server") : a.charset;
        _variables.set("", "character_set_client", charset, false, _warnings);
        _variables.set("", "character_set_results", charset, false, _warnings);
        _variables.set("", "character_set_connection", _variables.text("character_set_server"), false, _warnings);
        _variables.set("", "collation_connection", _variables.text("collation_server"), false, _warnings);
        break;
      }
    }
  }
}

void Session::check_database_name(const std::string& name) const {
  size_t characters = 0;
  for (const char ch : name)
    if ((static_cast<uint8_t>(ch) & 0xc0) != 0x80) ++characters;
  if (characters > 64) throw SqlError(1059, "42000", "Identifier name '" + name + "' is too long");
  if (name.empty() || name.back() == ' ') throw SqlError(1102, "42000", "Incorrect database name '" + name + "'");
  if (sql::same_word(name, "information_schema"))
    throw SqlError(1044, "42000", "Access denied for user '" + _info.user + "'@'" + _info.host + "' to database '" + name + "'");
}

Outcome Session::create_database(const sql::Statement& statement) {
  const auto& name = statement.database;
  check_database_name(name);
  // The character set and collation: as given, else the server's defaults.
  std::string charset = statement.charset;
  std::string collation = statement.collation;
  if (!charset.empty()) {
    if (charset_not_converted(charset))
      throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'character set " + lower(charset) + "'");
    if (!default_collation(charset)) throw SqlError(1115, "42000", "Unknown character set: '" + charset + "'");
    charset = default_collation(charset)->charset;
  }
  if (!collation.empty()) {
    const auto* info = find_collation(collation);
    const std::string against = charset.empty() ? _variables.text("character_set_server") : charset;
    if (!info) {
      if (charset_not_converted(std::string_view(collation).substr(0, collation.find('_'))))
        throw SqlError(1253, "42000", "COLLATION '" + collation + "' is not valid for CHARACTER SET '" + against + "'");
      throw SqlError(1273, "HY000", "Unknown collation: '" + collation + "'");
    }
    if (!charset.empty() && info->charset != charset)
      throw SqlError(1253, "42000", "COLLATION '" + collation + "' is not valid for CHARACTER SET '" + charset + "'");
    charset = info->charset;
    collation = info->name;
  } else if (!charset.empty()) {
    collation = default_collation(charset)->name;
  } else {
    charset = _variables.text("character_set_server");
    collation = _variables.text("collation_server");
  }
  // One row affected even when IF NOT EXISTS finds it there (observed).
  Outcome outcome;
  outcome.affected_rows = 1;
  if (!_catalog.create_database({name, charset, collation})) {
    const std::string message = "Can't create database '" + name + "'; database exists";
    if (!statement.if_exists) throw SqlError(1007, "HY000", message);
    _warnings.push_back({"Note", 1007, message});
  }
  return outcome;
}

Outcome Session::drop_database(const sql::Statement& statement) {
  const auto& name = statement.database;
  check_database_name(name);
  // A row affected for each table dropped with it (observed).
  Outcome outcome;
  size_t tables = 0;
  std::vector<uint64_t> ids;
  for (const auto& table : _catalog.table_names(name).value_or(std::vector<std::string>{}))
    if (const auto found = _catalog.table({name, table})) ids.push_back(found->id);
  if (_catalog.database(name))
    for (const auto id : ids) _rows.drop_table(id);
  if (!_catalog.drop_database(name, &tables)) {
    const std::string message = "Can't drop database '" + name + "'; database doesn't exist";
    if (!statement.if_exists) throw SqlError(1008, "HY000", message);
    _warnings.push_back({"Note", 1008, message});
  } else if (_database == name) {
    _database.reset();
  }
  outcome.affected_rows = tables;
  return outcome;
}

Outcome Session::show_databases(const sql::Statement& statement) {
  ResultType type;
  type.field_type = f::var_string;
  type.collation = protocol::collation::utf8mb4_0900_ai_ci;
  type.length = 256;
  type.decimals = 0;
  type.flags = c::not_null | c::binary | c::no_default_value;
  ResultSet result;
  result.columns.push_back({statement.like ? "Database (" + *statement.like + ")" : "Database", type});
  // Names are matched as bytes, as they are compared.
  const auto* bytes = collation_by_id(46);
  for (const auto& name : _catalog.database_names())
    if (!statement.like || bytes->like(name, *statement.like, "\\")) result.rows.push_back({name});
  Outcome outcome;
  outcome.result = std::move(result);
  return outcome;
}

Outcome Session::create_table(const sql::Statement& statement) {
  const auto& spec = statement.create_table;
  const auto& name = spec.name.back();
  const auto database = database_of(spec.name);
  check_table_name(name);
  if (sql::same_word(database, "information_schema"))
    throw SqlError(1044, "42000", "Access denied for user '" + _info.user + "'@'" + _info.host + "' to database '" + database + "'");
  // The deprecations come as the statement is read, whatever follows.
  type_warnings(spec, _warnings);
  const auto held = _catalog.database(database);
  if (!held) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
  const std::string exists = "Table '" + name + "' already exists";
  const auto already = [&]() -> Outcome {
    if (!spec.if_not_exists) throw SqlError(1050, "42S01", exists);
    _warnings.push_back({"Note", 1050, exists});
    return {};
  };
  // IF NOT EXISTS finds it before the definition is looked at (observed).
  if (_catalog.table({database, name})) return already();
  TableContext context;
  context.database = database;
  context.charset = held->charset;
  context.collation = held->collation;
  context.no_engine_substitution = has_mode(_variables.text("sql_mode"), "NO_ENGINE_SUBSTITUTION");
  context.eval = eval_settings();
  auto table = define_table(spec, context, _warnings);
  switch (_catalog.create_table(database, std::move(table))) {
    case catalog::Catalog::Created::created:
      return {};
    case catalog::Catalog::Created::exists:
      return already();
    case catalog::Catalog::Created::no_database:
      throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
  }
  return {};
}

Outcome Session::drop_table(const sql::Statement& statement) {
  std::vector<catalog::TableName> names;
  for (const auto& written : statement.tables) {
    catalog::TableName name{database_of(written), written.back()};
    if (std::find(names.begin(), names.end(), name) != names.end()) throw SqlError(1066, "42000", "Not unique table/alias: '" + name.name + "'");
    names.push_back(std::move(name));
  }
  // Their rows go with them.
  std::vector<uint64_t> ids;
  for (const auto& name : names)
    if (const auto found = _catalog.table(name)) ids.push_back(found->id);
  const auto missing = _catalog.drop_tables(names, statement.if_exists);
  if (missing.empty() || statement.if_exists)
    for (const auto id : ids) _rows.drop_table(id);
  if (!missing.empty()) {
    if (!statement.if_exists) {
      std::string list;
      for (const auto& m : missing) list += (list.empty() ? "" : ",") + m.database + "." + m.name;
      throw SqlError(1051, "42S02", "Unknown table '" + list + "'");
    }
    for (const auto& m : missing) _warnings.push_back({"Note", 1051, "Unknown table '" + m.database + "." + m.name + "'"});
  }
  return {};
}

Outcome Session::show_tables(const sql::Statement& statement) {
  const std::string database = statement.database.empty() ? database_of({""}) : statement.database;
  const auto names = _catalog.table_names(database);
  if (!names) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
  ResultType type;
  type.field_type = f::var_string;
  type.collation = protocol::collation::utf8mb4_0900_ai_ci;
  type.length = 256;
  type.flags = c::not_null | c::binary | c::no_default_value;
  ResultSet result;
  const std::string column = "Tables_in_" + database;
  result.columns.push_back({statement.like ? column + " (" + *statement.like + ")" : column, type});
  if (statement.full) {
    ResultType kind;
    kind.field_type = f::string;
    kind.collation = protocol::collation::utf8mb4_0900_ai_ci;
    kind.length = 44;
    kind.flags = c::not_null | c::binary | c::enum_ | c::no_default_value;
    result.columns.push_back({"Table_type", kind});
  }
  // Names are matched as bytes, as they are compared.
  const auto* bytes = collation_by_id(46);
  for (const auto& name : *names) {
    if (statement.like && !bytes->like(name, *statement.like, "\\")) continue;
    std::vector<std::optional<std::string>> row{name};
    if (statement.full) row.emplace_back("BASE TABLE");
    result.rows.push_back(std::move(row));
  }
  Outcome outcome;
  outcome.result = std::move(result);
  return outcome;
}

}  // namespace kvasir::exec
