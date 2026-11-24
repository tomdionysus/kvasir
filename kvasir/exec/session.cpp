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
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "kvasir/exec/aggregate.h"
#include "kvasir/exec/collation.h"
#include "kvasir/exec/ddl.h"
#include "kvasir/exec/rows.h"
#include "kvasir/exec/store.h"
#include "kvasir/exec/temporal.h"
#include "kvasir/protocol/messages.h"
#include "kvasir/protocol/types.h"
#include "kvasir/sql/keywords.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;

// What MySQL holds its own text in: system variables, session functions and
// SHOW's columns in utf8mb3_general_ci, names in utf8mb3_bin (observed).
constexpr uint16_t system_collation = 33;
constexpr uint16_t name_collation = 83;

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

// Whether a condition (WHERE, HAVING) holds: true only for a true value, a
// string read as a number (warning 1292 for what is left over).
bool holds(Evaluator& evaluator, const sql::Expr& condition) {
  const auto b = evaluator.truth(evaluator.value(condition), evaluator.type(condition));
  return b && *b;
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
    // Reported as 268435440 under utf8mb4 results (observed).
    out.field_type = f::long_blob;
    out.collation = t.collation;
    out.length = 67108860;
  } else {
    out.field_type = f::medium_blob;
    out.collation = protocol::collation::binary;
    out.length = 16777215;
    out.flags = c::binary;
  }
  return out;
}

uint64_t integer_text(const sql::Expr& e) {
  uint64_t v = 0;
  const auto r = std::from_chars(e.value.data(), e.value.data() + e.value.size(), v);
  if (r.ec != std::errc()) return UINT64_MAX;
  return v;
}

// An ORDER BY or GROUP BY item that names a select item by its position: an
// integer literal that fits 64 bits. A longer one is a constant, as MySQL
// reads it as a DECIMAL (observed on 8.4.13: ORDER BY 99999999999999999999
// sorts nothing).
bool is_position(const sql::Expr& e) {
  uint64_t v = 0;
  return e.kind == sql::ExprKind::integer && std::from_chars(e.value.data(), e.value.data() + e.value.size(), v).ec == std::errc();
}

// A string parameter as MySQL types it: 16383 characters of the
// connection's character set (observed: 65532 under utf8mb4).
ResultType string_parameter(const EvalSettings& e) {
  ResultType t;
  t.field_type = f::var_string;
  t.collation = e.result_collation;
  t.length = 16383 * e.result_max_bytes;
  t.decimals = protocol::not_fixed_decimals;
  t.flags = static_cast<uint16_t>(e.result_collation_binary ? c::binary : 0);
  return t;
}

ResultType longlong_parameter(bool is_unsigned) {
  ResultType t;
  t.field_type = f::longlong;
  t.collation = protocol::collation::binary;
  t.length = 21;
  t.precision = 20;
  t.flags = static_cast<uint16_t>(c::binary | (is_unsigned ? c::unsigned_ : 0));
  return t;
}

bool is_integer_kind(catalog::TypeKind k) {
  using K = catalog::TypeKind;
  return k == K::tinyint || k == K::smallint || k == K::mediumint || k == K::int_ || k == K::bigint;
}

// Calls `f` on every expression in the tree under `e`, `e` included.
template <typename F>
void each_expr(const sql::Expr* e, F&& f) {
  if (!e) return;
  f(*e);
  for (const auto& a : e->args) each_expr(a.get(), f);
  each_expr(e->operand.get(), f);
  each_expr(e->otherwise.get(), f);
}

// A SELECT stopped once its result is typed, while being prepared.
Outcome described(ResultSet result) {
  Outcome o;
  o.result = std::move(result);
  return o;
}

}  // namespace

uint64_t Session::limit_of(const sql::Expr& e) const {
  if (e.kind != sql::ExprKind::placeholder) return integer_text(e);
  // What COM_STMT_EXECUTE bound, as MySQL 8.4.13 reads it (observed): NULL
  // is 0; a negative integer is out of range; a DOUBLE or DECIMAL is
  // refused; a string is read as strtoull reads it, without a warning.
  const auto& v = _parameters->at(std::stoul(e.value)).value;
  if (std::holds_alternative<std::monostate>(v)) return 0;
  if (const auto* u = std::get_if<uint64_t>(&v)) return *u;
  if (const auto* i = std::get_if<int64_t>(&v)) {
    if (*i >= 0) return static_cast<uint64_t>(*i);
    throw SqlError(er_data_out_of_range, "22003", "signed integer value is out of range in 'mysqld_stmt_execute'");
  }
  const auto* s = std::get_if<std::string>(&v);
  if (!s || _parameters->at(std::stoul(e.value)).type.field_type == f::newdecimal)
    throw SqlError(er_wrong_arguments, "HY000", "Incorrect arguments to mysqld_stmt_execute");
  // Leading blanks, a sign, then digits; a minus wraps, past 2^64 is the
  // largest.
  size_t at = 0;
  while (at < s->size() && std::isspace(static_cast<unsigned char>((*s)[at]))) ++at;
  bool negative = false;
  if (at < s->size() && ((*s)[at] == '-' || (*s)[at] == '+')) negative = (*s)[at++] == '-';
  uint64_t n = 0;
  for (; at < s->size() && (*s)[at] >= '0' && (*s)[at] <= '9'; ++at) {
    const auto digit = static_cast<uint64_t>((*s)[at] - '0');
    if (n > (std::numeric_limits<uint64_t>::max() - digit) / 10) return std::numeric_limits<uint64_t>::max();
    n = n * 10 + digit;
  }
  return negative ? 0 - n : n;
}

Session::Session(const VariableRegistry& registry, catalog::Catalog& catalog, txn::Store& rows, SessionInfo info)
    : _registry(registry), _catalog(catalog), _rows(rows), _info(std::move(info)), _variables(registry) {
  take_client_collation();
}

void Session::take_client_collation() {
  // As SET NAMES <its character set> COLLATE <it> (observed); one whose
  // character set no session speaks leaves the server's.
  const auto* collation = find_collation(_info.client_collation);
  if (!collation || !default_collation(collation->charset)) return;
  std::vector<Warning> ignored;
  for (const char* name : {"character_set_client", "character_set_connection", "character_set_results"})
    _variables.set("", name, collation->charset, false, ignored);
  _variables.set("", "collation_connection", collation->name, false, ignored);
  _results_collation = collation->name;
}

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
  _results_collation.clear();
  take_client_collation();
  // COM_RESET_CONNECTION closes every prepared statement.
  _prepared.clear();
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
  // INSERT ... SELECT's source ends with the INSERT, a subquery with its
  // statement.
  if (_insert_source || _nested || _in_transaction || !_transaction) return;
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
  e.parameters = _parameters;
  e.div_precision_increment = static_cast<int>(_variables.number("div_precision_increment"));
  const auto* collation = find_collation(_variables.text("collation_connection"));
  e.result_collation = collation ? collation->id : protocol::collation::utf8mb4_0900_ai_ci;
  e.result_max_bytes = collation && collation->charset == "binary" ? 1 : 4;
  e.result_collation_binary = collation && collation->name.size() > 4 && collation->name.compare(collation->name.size() - 4, 4, "_bin") == 0;
  // The statement's time, and the session's zone: SYSTEM is the node's,
  // UTC (as system_time_zone says), else a +hh:mm offset.
  e.statement_us = _statement_us;
  e.zone_seconds = zone_seconds();
  const auto* clock = &wall_clock();
  e.clock_us = [clock] { return static_cast<int64_t>(clock->unix_us()); };
  e.queries = &_queries;
  e.outer = _outer;
  return e;
}

Outcome Session::nested_select(const sql::Select& query, const ColumnSource* outer, bool describe) {
  struct Restore {
    Session& s;
    const ColumnSource* outer;
    const View* view;
    bool describing;
    std::vector<Warning> warnings;
    ~Restore() {
      --s._nested;
      s._outer = outer;
      s._view = view;
      s._describing = describing;
      // The subquery's warnings join the statement's.
      warnings.insert(warnings.end(), s._warnings.begin(), s._warnings.end());
      s._warnings = std::move(warnings);
    }
  } restore{*this, _outer, _view, _describing, _warnings};
  ++_nested;
  _outer = outer;
  _describing = _describing || describe;
  _warnings.clear();
  return select(query);
}

std::vector<ResultType> Session::Queries::query_types(const sql::Select& query, const ColumnSource* outer) {
  const auto out = _session.nested_select(query, outer, true);
  std::vector<ResultType> types;
  for (const auto& column : out.result->columns) types.push_back(column.type);
  return types;
}

const dannan::contract::WallClock& Session::wall_clock() const { return _info.clock ? *_info.clock : dannan::contract::system_wall_clock(); }

int32_t Session::zone_seconds() const {
  // SYSTEM is the node's zone, UTC (as system_time_zone says); else the
  // variable holds a +hh:mm offset.
  const auto zone = _variables.text("time_zone");
  if (zone.size() != 6 || (zone[0] != '+' && zone[0] != '-')) return 0;
  const int32_t seconds = ((zone[1] - '0') * 10 + (zone[2] - '0')) * 3600 + ((zone[4] - '0') * 10 + (zone[5] - '0')) * 60;
  return zone[0] == '-' ? -seconds : seconds;
}

std::string Session::clock_text(const catalog::Column& column) const {
  // A TIMESTAMP holds UTC; a DATETIME the session's own time.
  const int64_t zone = column.type.kind == catalog::TypeKind::timestamp ? 0 : zone_seconds();
  auto m = temporal::from_unix_us(_statement_us + zone * 1000000);
  uint32_t step = 1;
  for (int i = column.type.scale; i < 6; ++i) step *= 10;
  m.micro -= m.micro % step;
  return temporal::text(m, temporal::Kind::datetime, column.type.scale);
}

void Session::stamp(const catalog::Table& table, const txn::Row& before, txn::Row& after, const std::vector<bool>& set) const {
  if (after == before) return;
  for (size_t i = 0; i < table.columns.size(); ++i)
    if (table.columns[i].on_update && !set[i]) after[i] = clock_text(table.columns[i]);
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
  // Held in utf8mb3_general_ci, as MySQL's are.
  return _variables.type_of(scope, name, system_collation, 3);
}

std::optional<catalog::Table> Session::named_table(const std::vector<std::string>& name) const {
  const auto database = database_of(name);
  auto table = _catalog.table({database, name.back()});
  if (!table) {
    if (!_catalog.database(database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
    throw SqlError(1146, "42S02", "Table '" + database + "." + name.back() + "' doesn't exist");
  }
  return table;
}

std::vector<ResultType> Session::infer_parameters(const sql::Statement& s) const {
  using K = sql::ExprKind;
  const auto e = eval_settings();
  // As MySQL types them at prepare (observed): an integer beside an integer
  // column or literal, LIMIT's unsigned, an INSERT's or UPDATE's by its
  // column's kind; anything else a string.
  std::vector<ResultType> out(s.placeholders, string_parameter(e));
  const auto set = [&](const sql::Expr* x, const ResultType& t) {
    if (x && x->kind == K::placeholder) out.at(std::stoul(x->value)) = t;
  };
  std::optional<catalog::Table> table;
  if (s.kind == sql::Statement::Kind::select && s.select.from.size() == 1 && !s.select.from[0].is_join() && !s.select.from[0].derived)
    table = _catalog.table({database_of(s.select.from[0].name), s.select.from[0].name.back()});
  if (s.kind == sql::Statement::Kind::insert) table = named_table(s.insert.table);
  if ((s.kind == sql::Statement::Kind::update || s.kind == sql::Statement::Kind::delete_) && !s.change.multiple()) table = named_table(s.change.table.name);
  const auto integer_column = [&](const std::string& name) {
    if (!table) return false;
    for (const auto& col : table->columns)
      if (sql::same_word(col.name, name)) return is_integer_kind(col.type.kind);
    return false;
  };
  const auto integer = [&](const sql::Expr& x) { return x.kind == K::integer || (x.kind == K::column && integer_column(x.path.back())); };
  const auto visit = [&](const sql::Expr* root) {
    each_expr(root, [&](const sql::Expr& x) {
      if (x.kind != K::binary || x.args.size() != 2) return;
      if (integer(*x.args[1])) set(x.args[0].get(), longlong_parameter(false));
      if (integer(*x.args[0])) set(x.args[1].get(), longlong_parameter(false));
    });
  };
  for (const auto& item : s.select.items) visit(item.expr.get());
  visit(s.select.where.get());
  visit(s.select.having.get());
  visit(s.change.where.get());
  for (const auto* limit : {s.select.limit.get(), s.select.offset.get(), s.change.limit.get()}) set(limit, longlong_parameter(true));
  if (s.kind == sql::Statement::Kind::insert && table) {
    for (const auto& row : s.insert.rows)
      for (size_t i = 0; i < row.size(); ++i) {
        const std::string name =
            s.insert.has_columns ? (i < s.insert.columns.size() ? s.insert.columns[i] : "") : (i < table->columns.size() ? table->columns[i].name : "");
        if (integer_column(name)) set(row[i].get(), longlong_parameter(false));
      }
  }
  // An unknown column SET names is refused at the prepare (observed); a
  // qualifier that names another table, at the execute.
  for (const auto& [column, value] : s.change.set) {
    if (table && !table->column(column.back())) {
      std::string name;
      for (size_t k = 0; k < column.size(); ++k) name += (k ? "." : "") + column[k];
      throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + name + "' in 'field list'");
    }
    if (integer_column(column.back())) set(value.get(), longlong_parameter(false));
  }
  return out;
}

Prepared Session::prepare(sql::Statement statement) {
  _previous = std::move(_warnings);
  _warnings.clear();
  try {
    if (_prepared.size() >= max_prepared_statements)
      throw SqlError(1461, "42000",
                     "Can't create more than max_prepared_stmt_count statements (current value: " + std::to_string(max_prepared_statements) + ")");
    if (!statement.unsupported.empty())
      throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support '" + statement.unsupported + "'");
    // A SELECT source's parameters are not yet typed at the prepare.
    if (statement.kind == sql::Statement::Kind::insert && statement.insert.select)
      throw SqlError(er_not_supported_yet, "42000",
                     std::string("This version of Kvasir doesn't yet support '") + (statement.insert.replace ? "REPLACE" : "INSERT") + " ... SELECT'");
    auto params = infer_parameters(statement);
    Prepared p;
    // Each parameter as the client is told it; a NULL bound later takes it.
    const auto results = results_collation();
    for (const auto& t : params) p.params.push_back({"?", reported(t, results)});
    if (statement.kind == sql::Statement::Kind::select) {
      // Typed with each parameter as prepared, and stopped before any row.
      std::vector<Parameter> unbound;
      for (const auto& t : params) unbound.push_back({t, Value{}});
      _parameters = &unbound;
      _describing = true;
      struct Restore {
        Session& s;
        ~Restore() {
          s._parameters = nullptr;
          s._describing = false;
        }
      } restore{*this};
      auto outcome = select(statement.select);
      for (auto& column : outcome.result->columns) column.type = reported(column.type, results);
      p.columns = std::move(outcome.result->columns);
    }
    _warnings.clear();
    p.id = _next_prepared++;
    _prepared.emplace(p.id, PreparedStatement{std::move(statement), std::move(params)});
    return p;
  } catch (const SqlError& e) {
    _warnings.push_back({"Error", e.code(), e.what()});
    throw;
  }
}

const std::vector<ResultType>& Session::parameter_types(uint32_t id, const std::string& command) const {
  const auto it = _prepared.find(id);
  if (it == _prepared.end()) throw SqlError(1243, "HY000", "Unknown prepared statement handler (" + std::to_string(id) + ") given to " + command);
  return it->second.params;
}

std::vector<Parameter> Session::bind(uint32_t id, const std::vector<protocol::BoundValue>& values) const {
  const auto& prepared = parameter_types(id, "mysqld_stmt_execute");
  const auto e = eval_settings();
  std::vector<Parameter> out;
  for (size_t i = 0; i < values.size() && i < prepared.size(); ++i) {
    const auto& v = values[i];
    const auto type = v.type.type;
    Parameter p;
    if (std::holds_alternative<std::monostate>(v.value)) {
      p.type = prepared[i];
    } else if (type == f::date || type == f::datetime || type == f::timestamp || type == f::time) {
      // A DATE stays one; a DATETIME, TIMESTAMP or TIME takes six fraction
      // digits whatever it holds (observed).
      const auto kind = type == f::date ? temporal::Kind::date : type == f::time ? temporal::Kind::time : temporal::Kind::datetime;
      p.type = temporal_result_type(kind, kind == temporal::Kind::date ? 0 : 6, false);
      p.value = protocol::temporal_text(type, std::get<std::string>(v.value));
    } else if (const auto* i64 = std::get_if<int64_t>(&v.value)) {
      p.type = longlong_parameter(false);
      p.value = *i64;
    } else if (const auto* u64 = std::get_if<uint64_t>(&v.value)) {
      p.type = longlong_parameter(true);
      p.value = *u64;
    } else if (const auto* d = std::get_if<double>(&v.value)) {
      p.type = double_type(23, false);
      p.type.flags = c::binary;
      p.value = *d;
    } else {
      const auto& bytes = std::get<std::string>(v.value);
      if (type == f::decimal || type == f::newdecimal) {
        // Read as its own digits (observed: 1.25 shows as 1.25).
        p.type = decimal_type(65, 30, false);
        p.type.flags = c::binary;
        p.type.own_scale = true;
        const auto parsed = Decimal::parse(bytes);
        p.value = parsed ? Value{*parsed} : Value{bytes};
      } else {
        p.type = string_parameter(e);
        p.value = bytes;
      }
    }
    out.push_back(std::move(p));
  }
  return out;
}

Outcome Session::execute_prepared(uint32_t id, const std::vector<Parameter>& parameters) {
  const auto it = _prepared.find(id);
  if (it == _prepared.end()) {
    const SqlError unknown(1243, "HY000", "Unknown prepared statement handler (" + std::to_string(id) + ") given to mysqld_stmt_execute");
    _previous = std::move(_warnings);
    _warnings = {{"Error", unknown.code(), unknown.what()}};
    throw unknown;
  }
  _parameters = &parameters;
  struct Restore {
    Session& s;
    ~Restore() { s._parameters = nullptr; }
  } restore{*this};
  return execute(it->second.statement);
}

void Session::close_prepared(uint32_t id) { _prepared.erase(id); }

std::optional<uint16_t> Session::results_collation() const {
  const auto results = _variables.get("", "character_set_results");
  if (is_null(results)) return std::nullopt;
  const auto charset = _variables.text("character_set_results");
  // SET NAMES ... COLLATE names the results' collation too (observed).
  if (const auto* named = find_collation(_results_collation); named && sql::same_word(named->charset, charset)) return named->id;
  const auto* d = default_collation(charset);
  return d ? d->id : protocol::collation::utf8mb4_0900_ai_ci;
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

// A session function's or SHOW's text, held in utf8mb3_general_ci as MySQL's
// is.
ResultType text_type(const EvalSettings& e, uint32_t characters, bool not_null) {
  ResultType t;
  t.field_type = f::var_string;
  t.collation = system_collation;
  t.length = characters * 3;
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
  // NOW() and its kin read the statement's start, fixed for its length.
  _statement_us = static_cast<int64_t>(wall_clock().unix_us());
  try {
    auto outcome = run(statement);
    if (outcome.result) {
      _row_count = -1;
      _found_rows = outcome.result->rows.size();
      // Each column as the client is told it: in character_set_results.
      const auto results = results_collation();
      for (auto& column : outcome.result->columns) column.type = reported(column.type, results);
    } else {
      _row_count = static_cast<int64_t>(outcome.affected_rows);
    }
    return outcome;
  } catch (const SqlError& e) {
    _warnings.insert(_warnings.end(), e.before().begin(), e.before().end());
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
    case Kind::select: {
      // An alias MySQL changes is warned of as the statement is read, before
      // anything the SELECT itself warns of.
      std::vector<Warning> names;
      for (const auto& item : statement.select.items)
        if (item.has_alias)
          if (auto w = alias_warning(item.alias)) names.push_back(std::move(*w));
      auto out = select(statement.select);
      _warnings.insert(_warnings.begin(), names.begin(), names.end());
      return out;
    }
    case Kind::show_warnings:
      return show_warnings(statement);
    case Kind::set:
      set(statement.assignments);
      return {};
    case Kind::use:
      if (is_information_schema(statement.database)) {
        _database = std::string(information_schema_name);
        follow_database();
        return {};
      }
      if (!_catalog.database(statement.database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + statement.database + "'");
      _database = statement.database;
      follow_database();
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
    // 1054 quotes the clause; 1052 does not (observed).
    const bool ambiguous = e.code() == er_ambiguous_field;
    if (e.code() != er_bad_field_error && !ambiguous) throw;
    std::string message = e.what();
    const std::string field = ambiguous ? " in field list " : "'field list'";
    const std::string named = ambiguous ? std::string(" in ") + clause + " " : std::string("'") + clause + "'";
    if (const auto at = message.rfind(field); at != std::string::npos) message.replace(at, field.size(), named);
    throw SqlError(e.code(), e.sqlstate(), message);
  }
}

bool holds_a_key(const catalog::Table& table, const std::vector<size_t>& listed);

// The columns grouping reads, each by a number: one table's, or a join's.
class IndexedColumns : public ColumnSource {
 public:
  virtual std::optional<size_t> index(const std::vector<std::string>& path) const = 0;
  // As a result column reports it, named `name`.
  virtual Column column(size_t i, std::string name) const = 0;
  virtual Value value_at(size_t i) const = 0;
  virtual std::optional<std::string> text(size_t i) const = 0;
  virtual uint16_t collation_at(size_t i) const = 0;
  // Its own name, and as 1055 names it: schema.table.column.
  virtual const std::string& name_of(size_t i) const = 0;
  virtual std::string named(size_t i) const = 0;
  // What `*` or `table.*` names (1051 for a table not here).
  virtual std::vector<size_t> star(const std::vector<std::string>& path) const = 0;
  // `known` columns and those that follow from them, as ONLY_FULL_GROUP_BY
  // finds them: a table one of whose keys is known is known whole.
  virtual std::vector<size_t> determined(std::vector<size_t> known) const = 0;
  // Whether grouping by these columns reads the groups off an index, with
  // no temporary table.
  virtual bool grouped_by_index(const std::vector<size_t>& by) const = 0;
};

// One table's columns, as a statement over it names them: column,
// table.column (by its alias when it has one) or schema.table.column, read
// from the row at hand.
class TableColumns : public IndexedColumns {
 public:
  // With `view`, information_schema's: its own types, and columns refused
  // when named.
  TableColumns(const catalog::Table& table, std::string database, std::string alias, const EvalSettings& eval, const View* view = nullptr, bool ordered = false)
      : _table(table), _database(std::move(database)), _alias(std::move(alias)), _eval(eval), _view(view), _view_ordered(ordered) {}

  std::optional<size_t> index(const std::vector<std::string>& path) const override {
    // information_schema's names match in any case; a table's as bytes.
    const auto same = [&](std::string_view a, std::string_view b) { return _view ? sql::same_word(a, b) : a == b; };
    if (path.size() == 2 && !same(path[0], qualifier())) return std::nullopt;
    if (path.size() == 3 && (!_alias.empty() || !same(path[0], _database) || !same(path[1], _table.name))) return std::nullopt;
    return _table.column(path.back());
  }

  std::optional<Found> find_column(const std::vector<std::string>& path) const override {
    const auto i = index(path);
    if (!i) return std::nullopt;
    return Found{type_at(*i, false), column_collation_id(_table.columns[*i])};
  }

  // The column's type: as an expression reads it, or (`output`) as a result
  // reports it, which for a view read through a temporary table is that
  // table's field (observed: MySQL types an expression from the view's own
  // columns). A refused column throws.
  ResultType type_at(size_t i, bool output = true) const {
    if (!_view) return column_result_type(_table, i, _eval);
    if (!_view->refused[i].empty()) throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support '" + _view->refused[i] + "'");
    return output && _view_ordered ? _view->ordered[i] : _view->types[i];
  }

  Value column_value(const std::vector<std::string>& path) const override {
    const auto i = index(path);
    if (!i || !_row || !(*_row)[*i]) return {};
    return value_of(_table.columns[*i].type, held(*i));
  }

  std::string written(const std::vector<std::string>& path) const override {
    const auto i = index(path);
    if (!i) return ColumnSource::written(path);
    return "`" + _database + "`.`" + qualifier() + "`.`" + _table.columns[*i].name + "`";
  }

  void set_row(const txn::Row* row) { _row = row; }

  Value value_at(size_t i) const override {
    if (!_row || !(*_row)[i]) return {};
    return value_of(_table.columns[i].type, held(i));
  }
  // What the column's values compare under: its collation, or 0 for a
  // number.
  uint16_t collation_at(size_t i) const override { return column_collation_id(_table.columns[i]); }

  const std::string& name_of(size_t i) const override { return _table.columns[i].name; }
  std::string named(size_t i) const override { return _database + "." + qualifier() + "." + _table.columns[i].name; }

  std::vector<size_t> star(const std::vector<std::string>& p) const override {
    const bool ours = p.empty() || (p.size() == 1 && p[0] == qualifier()) || (p.size() == 2 && _alias.empty() && p[0] == _database && p[1] == _table.name);
    if (!ours) throw SqlError(1051, "42S02", "Unknown table '" + p.back() + "'");
    std::vector<size_t> out(_table.columns.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = i;
    return out;
  }

  std::vector<size_t> determined(std::vector<size_t> known) const override {
    if (holds_a_key(_table, known)) return star({});
    return known;
  }

  // Grouped by the leading columns of an index (in any order), MySQL reads
  // the groups off the index (observed: GROUP BY c, k with k the primary
  // key still uses a temporary table). A view has no index.
  bool grouped_by_index(const std::vector<size_t>& by) const override {
    if (_view) return false;
    std::vector<size_t> distinct;
    for (const auto i : by)
      if (std::find(distinct.begin(), distinct.end(), i) == distinct.end()) distinct.push_back(i);
    for (const auto& key : _table.keys) {
      bool leading = key.parts.size() >= distinct.size();
      for (size_t p = 0; leading && p < distinct.size(); ++p)
        leading = !key.parts[p].prefix && std::find(distinct.begin(), distinct.end(), key.parts[p].column) != distinct.end();
      if (leading) return true;
    }
    return false;
  }

  // A column's value as a client is sent it.
  std::optional<std::string> text(size_t i) const override {
    if (!_row || !(*_row)[i]) return std::nullopt;
    if (_view) return (*_row)[i];
    return display_text(_table.columns[i].type, held(i), column_result_type(_table, i, _eval).length);
  }

  // A column's stored text as the session reads it: a TIMESTAMP, held in
  // UTC, in the session's zone (observed).
  std::string held(size_t i) const {
    const auto& stored = *(*_row)[i];
    const auto& type = _table.columns[i].type;
    if (type.kind != catalog::TypeKind::timestamp || !_eval.zone_seconds || stored.starts_with("0000-00-00")) return stored;
    return temporal::shifted(stored, type.scale, _eval.zone_seconds);
  }

  // The name the table goes by in the statement.
  const std::string& qualifier() const { return _alias.empty() ? _table.name : _alias; }

  Column column(size_t i, std::string name) const override {
    Column out(std::move(name), type_at(i));
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
  const View* _view;
  // Whether MySQL would read the view through a temporary table.
  bool _view_ordered;
  const txn::Row* _row = nullptr;
};

// One table of a join: what it is, its columns as the statement names them,
// and the rows read from it.
struct JoinLeaf {
  catalog::Table table;
  std::string database;
  // A derived table's result, its rows read from here.
  std::unique_ptr<View> view;
  std::unique_ptr<TableColumns> columns;
  std::vector<txn::Row> rows;
  // Each row's key, for a table a locking read reads.
  std::vector<std::string> keys;
  // On the inner side of an outer join: its columns may be NULL whatever
  // their definition says.
  bool nullable = false;
};
using JoinLeaves = std::vector<std::unique_ptr<JoinLeaf>>;

struct JoinPlace {
  size_t leaf;
  size_t column;
  friend bool operator==(const JoinPlace&, const JoinPlace&) = default;
  friend auto operator<=>(const JoinPlace&, const JoinPlace&) = default;
};

// A column USING or NATURAL merged into another: in any scope holding the
// whole join [first, end), its bare name is `to`.
struct JoinLink {
  JoinPlace to;
  size_t first;
  size_t end;
};
using JoinLinks = std::map<JoinPlace, JoinLink>;

// The columns of the tables a join, or one side of it, holds: a name only
// one of them has is that table's, as is a name USING merged; a bare name
// two hold otherwise is ambiguous (1052). A qualified name is always its own
// table's column.
class JoinedColumns : public ColumnSource {
 public:
  using Place = JoinPlace;
  JoinedColumns(const JoinLeaves& leaves, const JoinLinks& links, size_t first, size_t end) : _leaves(leaves), _links(links), _first(first), _end(end) {}

  // Where a column's bare name leads in this scope.
  Place merged(Place p) const {
    for (;;) {
      const auto link = _links.find(p);
      if (link == _links.end() || link->second.first < _first || link->second.end > _end) return p;
      p = link->second.to;
    }
  }

  std::optional<Place> place(const std::vector<std::string>& path) const {
    std::optional<Place> found;
    for (size_t l = _first; l < _end; ++l) {
      const auto i = _leaves[l]->columns->index(path);
      if (!i) continue;
      const Place p = path.size() == 1 ? merged({l, *i}) : Place{l, *i};
      if (found && *found != p) {
        std::string name;
        for (size_t k = 0; k < path.size(); ++k) name += (k ? "." : "") + path[k];
        throw SqlError(er_ambiguous_field, "23000", "Column '" + name + "' in field list is ambiguous");
      }
      found = p;
    }
    return found;
  }

  // A column's type as an expression reads it: NOT NULL no more on an outer
  // join's inner side.
  ResultType type_at(Place p) const {
    auto type = _leaves[p.leaf]->columns->type_at(p.column, false);
    if (_leaves[p.leaf]->nullable) type.flags = static_cast<uint16_t>(type.flags & ~c::not_null);
    return type;
  }

  std::optional<Found> find_column(const std::vector<std::string>& path) const override {
    const auto p = place(path);
    if (!p) return std::nullopt;
    return Found{type_at(*p), _leaves[p->leaf]->columns->collation_at(p->column)};
  }
  Value column_value(const std::vector<std::string>& path) const override {
    const auto p = place(path);
    if (!p) return {};
    return _leaves[p->leaf]->columns->value_at(p->column);
  }
  std::string written(const std::vector<std::string>& path) const override {
    const auto p = place(path);
    if (!p) return ColumnSource::written(path);
    return _leaves[p->leaf]->columns->written(path);
  }

 private:
  const JoinLeaves& _leaves;
  const JoinLinks& _links;
  size_t _first;
  size_t _end;
};

// What ON DUPLICATE KEY UPDATE reads: the row a key already holds, named as
// the table's columns are; the row being inserted by the row alias
// (alias.column, or the alias's own column names) and by VALUES(column).
class UpsertColumns : public ColumnSource {
 public:
  UpsertColumns(const TableColumns& held, const catalog::Table& table, const EvalSettings& eval, const sql::Insert& in)
      : _held(held), _table(table), _eval(eval), _in(in) {}

  std::optional<Found> find_column(const std::vector<std::string>& path) const override {
    if (auto found = _held.find_column(path)) return found;
    const auto i = new_index(path);
    if (!i) return std::nullopt;
    return Found{column_result_type(_table, *i, _eval), column_collation_id(_table.columns[*i])};
  }
  Value column_value(const std::vector<std::string>& path) const override {
    if (_held.find_column(path)) return _held.column_value(path);
    const auto i = new_index(path);
    return i ? inserted_at(*i) : Value{};
  }
  std::string written(const std::vector<std::string>& path) const override { return _held.written(path); }
  std::optional<Found> inserted_column(const std::string& name) const override {
    const auto i = _table.column(name);
    if (!i) return std::nullopt;
    return Found{column_result_type(_table, *i, _eval), column_collation_id(_table.columns[*i])};
  }
  Value inserted_value(const std::string& name) const override {
    const auto i = _table.column(name);
    return i ? inserted_at(*i) : Value{};
  }

  void set_inserted(const txn::Row* row) { _inserted = row; }

 private:
  // A column of the inserted row by the row alias: alias.column, or a name
  // the alias gives a column, in the order the INSERT lists them (the
  // table's when it lists none).
  std::optional<size_t> new_index(const std::vector<std::string>& path) const {
    if (_in.row_alias.empty()) return std::nullopt;
    const auto& names = _in.row_alias_columns;
    const auto by_alias_name = [&](const std::string& n) -> std::optional<size_t> {
      for (size_t k = 0; k < names.size(); ++k) {
        if (!sql::same_word(names[k], n)) continue;
        if (_in.has_columns) return k < _in.columns.size() ? _table.column(_in.columns[k]) : std::nullopt;
        return k < _table.columns.size() ? std::optional<size_t>(k) : std::nullopt;
      }
      return std::nullopt;
    };
    if (path.size() == 2 && path[0] == _in.row_alias) return names.empty() ? _table.column(path[1]) : by_alias_name(path[1]);
    if (path.size() == 1 && !names.empty()) return by_alias_name(path[0]);
    return std::nullopt;
  }
  Value inserted_at(size_t i) const {
    if (!_inserted || !(*_inserted)[i]) return {};
    return value_of(_table.columns[i].type, *(*_inserted)[i]);
  }

  const TableColumns& _held;
  const catalog::Table& _table;
  EvalSettings _eval;
  const sql::Insert& _in;
  const txn::Row* _inserted = nullptr;
};

// Whether MySQL reads an information_schema view through a temporary table
// for this SELECT, which changes some columns' types (observed with ORDER
// BY; GROUP BY and DISTINCT taken as the same).
bool through_temporary_table(const sql::Select& select) { return !select.order.empty() || !select.group_by.empty() || select.distinct; }

// An expression over an information_schema view's columns as that temporary
// table's field reports it (observed with Connector/J's getTables and
// getColumns): a string with 0 decimals, any BLOB as BLOB; an integer
// without BINARY, a LONG below 10 digits' length, else a LONGLONG.
ResultType as_view_field(ResultType t) {
  if (is_string_type(t)) {
    t.decimals = 0;
    if (t.field_type == f::tiny_blob || t.field_type == f::medium_blob || t.field_type == f::long_blob || t.field_type == f::blob) {
      t.field_type = f::blob;
      t.flags = static_cast<uint16_t>(t.flags | c::blob);
    }
    return t;
  }
  t.flags = static_cast<uint16_t>(t.flags & ~c::binary);
  if (t.field_type == f::tiny || t.field_type == f::short_ || t.field_type == f::int24 || t.field_type == f::long_ || t.field_type == f::longlong)
    t.field_type = t.length >= 10 ? f::longlong : f::long_;
  return t;
}

// Whether an expression calls VALUES() anywhere in it.
bool mentions_values(const sql::Expr& e) {
  if (e.kind == sql::ExprKind::call && sql::same_word(e.value, "VALUES")) return true;
  for (const auto& a : e.args)
    if (a && mentions_values(*a)) return true;
  return (e.operand && mentions_values(*e.operand)) || (e.otherwise && mentions_values(*e.otherwise));
}

// Whether an expression names a column anywhere in it.
bool mentions_column(const sql::Expr& e) {
  // A subquery may read the outer row (a correlated one).
  if (e.kind == sql::ExprKind::column || e.kind == sql::ExprKind::subquery) return true;
  for (const auto& a : e.args)
    if (a && mentions_column(*a)) return true;
  return (e.operand && mentions_column(*e.operand)) || (e.otherwise && mentions_column(*e.otherwise));
}

// Text a column's character set cannot hold, as error 1366 shows it: from
// the first byte past ASCII, each such byte as \xHH, at most six, then
// "..." when more follow (observed).
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
  if (i < s.size()) out += "...";
  return out;
}

// A value as a column stores it, or the error MySQL gives a row for it
// (strict mode). A statement that ignores errors (INSERT IGNORE) is given
// each as a warning instead, and the value MySQL stores in its place
// (observed on MySQL 8.4.13).
std::string stored_text(const catalog::Column& column, const Value& value, const ResultType& from, size_t row_number, std::vector<Warning>& warnings,
                        bool ignore = false, bool field = false, int32_t zone_seconds = 0) {
  const std::string at_row = " at row " + std::to_string(row_number);
  const auto stored = store(column.type, value, from, zone_seconds);
  const auto value_text = [&]() { return to_text(value, from).value_or("NULL"); };
  const auto failed = [&](const char* level, uint16_t code, const char* sqlstate, const std::string& message) -> std::string {
    if (!ignore) throw SqlError(code, sqlstate, message);
    warnings.push_back({level, code, message});
    return stored.lenient;
  };
  const std::string truncated = "Data truncated for column '" + column.name + "'" + at_row;
  // A date or time names its type (observed: 1292 "Incorrect date value:
  // '2024-02-30' for column 'd' at row 1"; a DATETIME into a DATE a Note).
  if (catalog::is_temporal(column.type.kind)) {
    const char* word = column.type.kind == catalog::TypeKind::date ? "date" : column.type.kind == catalog::TypeKind::time ? "time" : "datetime";
    const auto incorrect = std::string("Incorrect ") + word + " value: '" + value_text() + "' for column '" + column.name + "'" + at_row;
    if (stored.result == Stored::Result::truncated) {
      warnings.push_back({"Note", er_truncated_wrong_value, incorrect});
      return stored.text;
    }
    // Ignoring the error stores the zero value, a TIME clamped, with 1264.
    if (stored.result != Stored::Result::stored) {
      if (ignore) return failed("Warning", 1264, "22003", "Out of range value for column '" + column.name + "'" + at_row);
      return failed("Warning", er_truncated_wrong_value, "22007", incorrect);
    }
  }
  switch (stored.result) {
    case Stored::Result::truncated:
      warnings.push_back({"Note", 1265, truncated});
      [[fallthrough]];
    case Stored::Result::stored:
      return stored.text;
    case Stored::Result::out_of_range:
      return failed("Warning", 1264, "22003", "Out of range value for column '" + column.name + "'" + at_row);
    case Stored::Result::too_long:
      // A column copied as it is (INSERT ... SELECT of a table's column) is
      // truncated, an expression too long (observed).
      if (ignore || field) return failed("Warning", 1265, "01000", truncated);
      throw SqlError(1406, "22001", "Data too long for column '" + column.name + "'" + at_row);
    case Stored::Result::garbage:
      if (column.type.kind != catalog::TypeKind::decimal) return failed("Warning", 1265, "01000", truncated);
      // A DECIMAL's leading number is kept with a note.
      if (ignore) return failed("Note", 1265, "01000", truncated);
      [[fallthrough]];
    case Stored::Result::invalid:
      break;
  }
  if (catalog::is_text(column.type.kind))
    return failed("Warning", 1366, "HY000", "Incorrect string value: '" + escaped_bytes(value_text()) + "' for column '" + column.name + "'" + at_row);
  return failed("Warning", 1366, "HY000",
                std::string("Incorrect ") + (column.type.kind == catalog::TypeKind::decimal ? "decimal" : "integer") + " value: '" + value_text() +
                    "' for column '" + column.name + "'" + at_row);
}

// A SELECT's result value read back from its text by its type: what INSERT
// ... SELECT stores.
Value value_from_text(const std::optional<std::string>& text, const ResultType& type) {
  if (!text) return {};
  if (!type.number_string) {
    switch (type.field_type) {
      case f::tiny:
      case f::short_:
      case f::int24:
      case f::long_:
      case f::longlong:
        if (type.is_unsigned()) return static_cast<uint64_t>(std::stoull(*text));
        return static_cast<int64_t>(std::stoll(*text));
      case f::newdecimal:
        if (const auto d = Decimal::parse(*text)) return *d;
        break;
      case f::float_:
      case f::double_:
        return std::strtod(text->c_str(), nullptr);
      default:
        break;
    }
  }
  return *text;
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
SqlError duplicate_error(const catalog::Table& table, std::optional<size_t> primary, const std::string& duplicate, const txn::Row& row) {
  const auto name = duplicate.substr(0, duplicate.find('\0'));
  const auto& key =
      duplicate.empty() ? table.keys[*primary] : *std::find_if(table.keys.begin(), table.keys.end(), [&](const catalog::Key& k) { return k.name == name; });
  return SqlError(1062, "23000", "Duplicate entry '" + key_text(table, key, row) + "' for key '" + table.name + "." + key.name + "'");
}

[[noreturn]] void duplicate_entry(const catalog::Table& table, std::optional<size_t> primary, const std::string& duplicate, const txn::Row& row) {
  throw duplicate_error(table, primary, duplicate, row);
}

// The collation an ORDER BY value sorts under: a column's own, else the
// results'.
uint16_t order_collation(const sql::Expr& e, const ResultType& type, const ColumnSource& columns, uint16_t results) {
  if (e.kind == sql::ExprKind::column)
    if (const auto found = columns.find_column(e.path)) return found->collation;
  return type.collation == protocol::collation::binary ? protocol::collation::binary : results;
}

// What two values of an expression compare equal under, for grouping and
// DISTINCT: a string's collation (a column's own), 0 for a number.
uint16_t key_collation(const sql::Expr& e, const ResultType& type, const ColumnSource* columns, uint16_t results) {
  if (!is_string_type(type)) return 0;
  if (e.kind == sql::ExprKind::column && columns)
    if (const auto found = columns->find_column(e.path)) return found->collation;
  return type.collation == protocol::collation::binary ? protocol::collation::binary : results;
}

// Rows of values, ordered as GROUP BY and DISTINCT tell them apart.
struct ValuesLess {
  const std::vector<uint16_t>* collations;
  bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
    for (size_t i = 0; i < a.size(); ++i) {
      const int order = compare_for_order(a[i], b[i], (*collations)[i]);
      if (order != 0) return order < 0;
    }
    return false;
  }
};

bool contains_aggregate(const sql::Expr& e) {
  if (is_aggregate(e)) return true;
  for (const auto& a : e.args)
    if (a && contains_aggregate(*a)) return true;
  return (e.operand && contains_aggregate(*e.operand)) || (e.otherwise && contains_aggregate(*e.otherwise));
}

// Whether a SELECT is grouped: GROUP BY, HAVING, or an aggregate in its list
// or ORDER BY.
bool is_grouped(const sql::Select& s) {
  if (!s.group_by.empty() || s.having || s.with_rollup) return true;
  for (const auto& item : s.items)
    if (item.expr && contains_aggregate(*item.expr)) return true;
  for (const auto& o : s.order)
    if (contains_aggregate(*o.expr)) return true;
  return false;
}

// The aggregate calls in an expression, the outermost.
void collect_aggregates(const sql::Expr& e, std::vector<const sql::Expr*>& out) {
  if (is_aggregate(e)) {
    if (std::find(out.begin(), out.end(), &e) == out.end()) out.push_back(&e);
    return;
  }
  for (const auto& a : e.args)
    if (a) collect_aggregates(*a, out);
  if (e.operand) collect_aggregates(*e.operand, out);
  if (e.otherwise) collect_aggregates(*e.otherwise, out);
}

// The columns an expression reads outside its aggregates.
void bare_columns(const sql::Expr& e, std::vector<const sql::Expr*>& out) {
  if (is_aggregate(e)) return;
  if (e.kind == sql::ExprKind::column) {
    out.push_back(&e);
    return;
  }
  for (const auto& a : e.args)
    if (a) bare_columns(*a, out);
  if (e.operand) bare_columns(*e.operand, out);
  if (e.otherwise) bare_columns(*e.otherwise, out);
}

// Whether two expressions are the same, a column by the column it names.
bool same_expr(const sql::Expr& a, const sql::Expr& b, const IndexedColumns* columns) {
  if (a.kind != b.kind) return false;
  if (a.kind == sql::ExprKind::column) {
    if (!columns) return false;
    const auto x = columns->index(a.path), y = columns->index(b.path);
    return x && y && *x == *y;
  }
  if (a.value != b.value || a.charset != b.charset || a.scope != b.scope || a.negated != b.negated || a.distinct != b.distinct || a.star != b.star ||
      a.args.size() != b.args.size() || !a.operand != !b.operand || !a.otherwise != !b.otherwise)
    return false;
  for (size_t i = 0; i < a.args.size(); ++i) {
    if (!a.args[i] != !b.args[i]) return false;
    if (a.args[i] && !same_expr(*a.args[i], *b.args[i], columns)) return false;
  }
  return (!a.operand || same_expr(*a.operand, *b.operand, columns)) && (!a.otherwise || same_expr(*a.otherwise, *b.otherwise, columns));
}

// The columns WHERE fixes to one value: `column = constant` in its top-level
// AND.
void fixed_columns(const sql::Expr& e, const IndexedColumns& columns, std::vector<size_t>& out) {
  if (e.kind != sql::ExprKind::binary) return;
  if (e.value == "AND") {
    fixed_columns(*e.args[0], columns, out);
    fixed_columns(*e.args[1], columns, out);
    return;
  }
  if (e.value != "=") return;
  for (size_t side = 0; side < 2; ++side) {
    const auto& column = *e.args[side];
    const auto& other = *e.args[1 - side];
    if (column.kind != sql::ExprKind::column || mentions_column(other) || contains_aggregate(other)) continue;
    if (const auto i = columns.index(column.path)) out.push_back(*i);
  }
}

// An expression as 1056 names it: as written.
std::string written(const sql::Expr& e) {
  if (e.kind == sql::ExprKind::column) {
    std::string out;
    for (size_t i = 0; i < e.path.size(); ++i) out += (i ? "." : "") + e.path[i];
    return out;
  }
  if (e.kind == sql::ExprKind::call) {
    std::string out = e.value + "(" + (e.distinct ? "DISTINCT " : "") + (e.star ? "*" : "");
    for (size_t i = 0; i < e.args.size(); ++i) out += (i ? ", " : "") + written(*e.args[i]);
    return out + ")";
  }
  return mysql_text(e);
}

// GROUP_FLAG, NUM's bit: a nullable column a temporary table groups by.
constexpr uint16_t group_flag = 32768;

// An expression's type as a temporary table's field reports it: a number
// without BINARY, a string with 0 decimals.
ResultType as_field(ResultType t) {
  if (is_string_type(t))
    t.decimals = 0;
  else
    t.flags = static_cast<uint16_t>(t.flags & ~c::binary);
  return t;
}

// Whether `listed` columns hold the whole of a key that tells rows apart:
// the primary key, or a unique key of NOT NULL columns, without prefixes.
bool holds_a_key(const catalog::Table& table, const std::vector<size_t>& listed) {
  for (const auto& key : table.keys) {
    bool whole = key.kind != catalog::Key::Kind::index;
    for (const auto& part : key.parts)
      whole = whole && !part.prefix && !table.columns[part.column].nullable && std::find(listed.begin(), listed.end(), part.column) != listed.end();
    if (whole) return true;
  }
  return false;
}

// A value's text, with ZEROFILL's zeros when its column says so.
std::optional<std::string> padded_text(const Value& v, const ResultType& type) {
  auto text = to_text(v, type);
  if (text && (type.flags & protocol::column_flag::zerofill) && text->size() < type.length) text->insert(0, type.length - text->size(), '0');
  return text;
}

// The aggregates of the group at hand.
class GroupAggregates : public AggregateSource {
 public:
  explicit GroupAggregates(const std::vector<AggregateSpec>& specs) : _specs(specs) {}

  std::optional<ResultType> aggregate_type(const sql::Expr& call) const override {
    if (const auto i = find(call)) return _specs[*i].type();
    return std::nullopt;
  }
  Value aggregate_value(const sql::Expr& call) const override {
    const auto i = find(call);
    if (!i || !_group) return {};
    return (*_group)[*i].result();
  }
  void set_group(const std::vector<Accumulator>* group) { _group = group; }

 private:
  std::optional<size_t> find(const sql::Expr& call) const {
    for (size_t i = 0; i < _specs.size(); ++i)
      if (&_specs[i].call() == &call) return i;
    return std::nullopt;
  }

  const std::vector<AggregateSpec>& _specs;
  const std::vector<Accumulator>* _group = nullptr;
};

// The names HAVING reads: a grouped column, a column the select list names
// bare, or a select item's alias (observed: anything else is 1054).
class HavingColumns : public ColumnSource {
 public:
  HavingColumns(const IndexedColumns* table, const sql::Select& select, const std::vector<const sql::Expr*>& groups, Evaluator& items, uint16_t results)
      : _table(table), _select(select), _groups(groups), _items(items), _results(results) {}

  std::optional<Found> find_column(const std::vector<std::string>& path) const override {
    const auto e = resolve(path);
    if (!e) return std::nullopt;
    if (!*e) return _table->find_column(path);
    const auto type = _items.type(**e);
    return Found{type, key_collation(**e, type, _table, _results)};
  }
  Value column_value(const std::vector<std::string>& path) const override {
    const auto e = resolve(path);
    if (!e) return {};
    if (!*e) return _table->column_value(path);
    return _items.value(**e);
  }
  std::string written(const std::vector<std::string>& path) const override {
    const auto e = resolve(path);
    return e && !*e && _table ? _table->written(path) : ColumnSource::written(path);
  }

 private:
  // Nothing for a name HAVING cannot read; a null expression for a table's
  // column; else the select item an alias names.
  std::optional<const sql::Expr*> resolve(const std::vector<std::string>& path) const {
    const sql::Expr* const table_column = nullptr;
    if (_table) {
      if (const auto i = _table->index(path)) {
        for (const auto* g : _groups)
          if (g->kind == sql::ExprKind::column && _table->index(g->path) == i) return table_column;
        for (const auto& item : _select.items)
          if (item.star || (item.expr->kind == sql::ExprKind::column && _table->index(item.expr->path) == i)) return table_column;
      }
    }
    if (path.size() == 1)
      for (const auto& item : _select.items)
        if (item.has_alias && sql::same_word(item.alias, path[0])) return item.expr.get();
    return std::nullopt;
  }

  const IndexedColumns* _table;
  const sql::Select& _select;
  const std::vector<const sql::Expr*>& _groups;
  Evaluator& _items;
  uint16_t _results;
};

}  // namespace

void Session::begin_reading(txn::Transaction& t) {
  // A subquery reads its statement's snapshot.
  if (_nested) return;
  // READ COMMITTED (and READ UNCOMMITTED, served as it) reads afresh each
  // statement; REPEATABLE READ keeps the snapshot of its first read.
  const auto level = _variables.text("transaction_isolation");
  _rows.take_snapshot(t, sql::same_word(level, "READ-COMMITTED") || sql::same_word(level, "READ-UNCOMMITTED"));
}

namespace {

// The primary-key range a WHERE confines a locking read to, as InnoDB's
// next-key locks cover the stretch of the index a condition reads: for a
// single-column, ascending, integer primary key compared with integer
// constants (=, <, <=, >, >=, BETWEEN, IN) joined by AND; every key for
// anything else, as a scan without an index locks. Never less than every key
// the WHERE can match.
txn::Store::KeyRange key_range(const sql::Expr* where, const catalog::Table& table, const TableColumns& columns, Evaluator& evaluator) {
  using Range = txn::Store::KeyRange;
  const auto primary = table.primary_key();
  if (!where || !primary || table.keys[*primary].parts.size() != 1 || table.keys[*primary].parts[0].descending) return {};
  const size_t pk = table.keys[*primary].parts[0].column;
  const auto& column = table.columns[pk];
  if (!catalog::is_integer(column.type.kind)) return {};
  // An integer constant's key, or nothing.
  const auto key_of = [&](const sql::Expr& e) -> std::optional<std::string> {
    if (mentions_column(e)) return std::nullopt;
    try {
      const auto t = evaluator.type(e);
      const bool integer = !t.number_string && (t.field_type == f::tiny || t.field_type == f::short_ || t.field_type == f::int24 || t.field_type == f::long_ ||
                                                t.field_type == f::longlong);
      if (!integer) return std::nullopt;
      const auto v = evaluator.value(e);
      if (!std::holds_alternative<int64_t>(v) && !std::holds_alternative<uint64_t>(v)) return std::nullopt;
      const auto stored = store(column.type, v, t);
      if (stored.result != Stored::Result::stored) return std::nullopt;
      txn::Row row(table.columns.size());
      row[pk] = stored.text;
      return encode_key(table, table.keys[*primary], row);
    } catch (const SqlError&) {
      return std::nullopt;
    }
  };
  const auto is_pk = [&](const sql::Expr& e) { return e.kind == sql::ExprKind::column && columns.index(e.path) == pk; };
  const auto intersect = [](Range a, const Range& b) {
    if (b.low && (!a.low || *b.low > *a.low || (*b.low == *a.low && !b.low_inclusive))) {
      a.low = b.low;
      a.low_inclusive = b.low_inclusive;
    }
    if (b.high && (!a.high || *b.high < *a.high || (*b.high == *a.high && !b.high_inclusive))) {
      a.high = b.high;
      a.high_inclusive = b.high_inclusive;
    }
    return a;
  };
  std::function<Range(const sql::Expr&)> of = [&](const sql::Expr& e) -> Range {
    if (e.kind == sql::ExprKind::binary && e.value == "AND") return intersect(of(*e.args[0]), of(*e.args[1]));
    if (e.kind == sql::ExprKind::binary && e.args.size() == 2) {
      std::string op = e.value;
      const sql::Expr* constant = nullptr;
      if (is_pk(*e.args[0])) {
        constant = e.args[1].get();
      } else if (is_pk(*e.args[1])) {
        constant = e.args[0].get();
        // c < pk is pk > c.
        if (op == "<")
          op = ">";
        else if (op == ">")
          op = "<";
        else if (op == "<=")
          op = ">=";
        else if (op == ">=")
          op = "<=";
      }
      if (!constant || (op != "=" && op != "<" && op != "<=" && op != ">" && op != ">=")) return {};
      const auto key = key_of(*constant);
      if (!key) return {};
      Range r;
      if (op == "=" || op == ">" || op == ">=") {
        r.low = key;
        r.low_inclusive = op != ">";
      }
      if (op == "=" || op == "<" || op == "<=") {
        r.high = key;
        r.high_inclusive = op != "<";
      }
      return r;
    }
    if (e.kind == sql::ExprKind::between && !e.negated && is_pk(*e.args[0])) {
      const auto low = key_of(*e.args[1]), high = key_of(*e.args[2]);
      Range r;
      r.low = low;
      r.high = high;
      return r;
    }
    if (e.kind == sql::ExprKind::in && !e.negated && is_pk(*e.args[0]) && e.args.size() > 1) {
      // Every value's key, the range from the least to the greatest.
      Range r;
      for (size_t i = 1; i < e.args.size(); ++i) {
        const auto key = key_of(*e.args[i]);
        if (!key) return {};
        if (!r.low || *key < *r.low) r.low = key;
        if (!r.high || *key > *r.high) r.high = key;
      }
      return r;
    }
    return {};
  };
  return of(*where);
}

}  // namespace

const sql::Select::Locking* Session::locking_clause(const sql::Select& select, const std::string& qualifier) const {
  return locking_clauses(select, {qualifier}).front();
}

std::vector<const sql::Select::Locking*> Session::locking_clauses(const sql::Select& select, const std::vector<std::string>& qualifiers) const {
  // INSERT ... SELECT reads its source with shared locks, but under READ
  // COMMITTED (the manual, "Locks Set by Different SQL Statements in
  // InnoDB").
  static const sql::Select::Locking source_lock;
  const auto level = _variables.text("transaction_isolation");
  std::vector<const sql::Select::Locking*> out(qualifiers.size(), nullptr);
  if (_insert_source && select.locking.empty()) {
    if (sql::same_word(level, "READ-COMMITTED") || sql::same_word(level, "READ-UNCOMMITTED")) return out;
    return std::vector<const sql::Select::Locking*>(qualifiers.size(), &source_lock);
  }
  // SERIALIZABLE reads every plain SELECT as FOR SHARE in a transaction (BEGIN,
  // or autocommit off); a SELECT that is its own transaction is a consistent
  // read (the manual, "Transaction Isolation Levels").
  if (select.locking.empty() && sql::same_word(level, "SERIALIZABLE") && (_in_transaction || !_variables.number("autocommit")))
    return std::vector<const sql::Select::Locking*>(qualifiers.size(), &source_lock);
  // Each OF names a table of the statement (3568); a table falls under one
  // clause at most (3569: named in backquotes when OF named it, observed).
  for (const auto& clause : select.locking)
    for (const auto& name : clause.of)
      if (std::find(qualifiers.begin(), qualifiers.end(), name) == qualifiers.end())
        throw SqlError(3568, "HY000", "Unresolved table name `" + name + "` in locking clause.");
  for (size_t k = 0; k < qualifiers.size(); ++k)
    for (const auto& clause : select.locking) {
      const bool named = std::find(clause.of.begin(), clause.of.end(), qualifiers[k]) != clause.of.end();
      if (!clause.of.empty() && !named) continue;
      if (out[k]) throw SqlError(3569, "HY000", "Table " + (named ? "`" + qualifiers[k] + "`" : qualifiers[k]) + " appears in multiple locking clauses.");
      out[k] = &clause;
    }
  return out;
}

std::vector<txn::Row> Session::locked_rows(const sql::Select::Locking& clause, txn::Transaction& t, const catalog::Table& table,
                                           const txn::Store::KeyRange& range, const std::function<bool(const txn::Row&)>& passes) {
  using Wait = sql::Select::Locking::Wait;
  const auto mode = clause.exclusive ? txn::Store::LockMode::exclusive : txn::Store::LockMode::shared;
  const auto blocked = [&](uint64_t holder) {
    if (clause.wait == Wait::nowait) throw SqlError(3572, "HY000", "Statement aborted because lock(s) could not be acquired immediately and NOWAIT is set.");
    throw LockWaitNeeded(t.id(), holder);
  };
  std::vector<txn::Row> out;
  std::vector<std::string> keys;
  for (auto& held : _rows.current(t, table.id)) {
    if (clause.wait == Wait::skip_locked && _rows.held_by_other(t, table.id, held.key, mode)) continue;
    if (passes(held.row)) keys.push_back(held.key);
    out.push_back(std::move(held.row));
  }
  uint64_t holder = 0;
  const auto level = _variables.text("transaction_isolation");
  const bool gaps = !sql::same_word(level, "READ-COMMITTED") && !sql::same_word(level, "READ-UNCOMMITTED");
  if (gaps && clause.wait != Wait::skip_locked && !_rows.lock_range(t, table.id, range, &holder)) blocked(holder);
  if (!_rows.lock_rows(t, table.id, keys, mode, &holder)) blocked(holder);
  return out;
}

std::string Session::database_of(const std::vector<std::string>& name) const {
  if (name.size() == 2) return name[0];
  if (!_database) throw SqlError(er_no_db_error, "3D000", "No database selected");
  return *_database;
}

namespace {

ResultType without_keys(ResultType t);

// A catalog type that reads a result column's text back as its value (a
// TIMESTAMP's text is already in the session's zone, so read as DATETIME).
catalog::ColumnType readable_type(const ResultType& t) {
  using K = catalog::TypeKind;
  catalog::ColumnType out;
  out.is_unsigned = t.is_unsigned();
  const bool binary = t.collation == protocol::collation::binary;
  switch (t.field_type) {
    case f::tiny:
      out.kind = K::tinyint;
      break;
    case f::short_:
      out.kind = K::smallint;
      break;
    case f::int24:
      out.kind = K::mediumint;
      break;
    case f::long_:
      out.kind = K::int_;
      break;
    case f::longlong:
      out.kind = K::bigint;
      break;
    case f::year:
      out.kind = K::year;
      break;
    case f::decimal:
    case f::newdecimal:
      out.kind = K::decimal;
      out.length = static_cast<uint32_t>(std::max(t.precision, 1));
      out.scale = t.decimals;
      break;
    case f::float_:
      out.kind = K::float_;
      break;
    case f::double_:
      out.kind = K::double_;
      break;
    case f::date:
      out.kind = K::date;
      break;
    case f::time:
      out.kind = K::time;
      out.scale = t.decimals;
      break;
    case f::datetime:
    case f::timestamp:
      out.kind = K::datetime;
      out.scale = t.decimals;
      break;
    case f::blob:
    case f::tiny_blob:
    case f::medium_blob:
    case f::long_blob:
      out.kind = binary ? K::longblob : K::longtext;
      break;
    default:
      out.kind = binary ? K::varbinary : K::varchar;
      out.length = binary ? t.length : t.length / widest_character(t.collation);
      break;
  }
  if (!binary && catalog::is_text(out.kind)) {
    if (const auto* c = find_collation(t.collation)) {
      out.charset = c->charset;
      out.collation = c->name;
    }
  }
  return out;
}

}  // namespace

View Session::derived_view(const sql::TableRef& from) {
  if (from.alias.empty()) throw SqlError(1248, "42000", "Every derived table must have its own alias");
  const auto& query = *from.derived;
  auto result = *select(query).result;
  const auto settings = eval_settings();
  View view;
  view.table.name = from.alias;
  // Each column as the outer query reports it (observed): MySQL merges a
  // derived table with a FROM into the outer query, so a column is its
  // inner item's, a signed integer literal one longer, an aggregate without
  // BINARY, a date or time under the results' collation and as long as its
  // characters there; one without FROM, or a set operation, is
  // materialised, an integer of nine digits or fewer a LONG.
  // Grouping, DISTINCT or LIMIT keep MySQL from merging (observed: the
  // columns then lose their keys and a number its BINARY).
  const bool grouped =
      std::any_of(query.items.begin(), query.items.end(), [](const sql::SelectItem& item) { return item.expr && contains_aggregate(*item.expr); });
  const bool merged = !query.set && !query.from.empty() && !query.distinct && query.group_by.empty() && !query.having && !query.limit && !grouped;
  const bool plain_items = std::none_of(query.items.begin(), query.items.end(), [](const sql::SelectItem& item) { return item.star; });
  for (size_t i = 0; i < result.columns.size(); ++i) {
    auto type = result.columns[i].type;
    const sql::Expr* item = plain_items && i < query.items.size() ? query.items[i].expr.get() : nullptr;
    const bool integer = type.field_type == f::longlong || type.field_type == f::long_;
    if (merged) {
      // An integer literal one longer than its digits (observed: 1 is 2).
      if (item && item->kind == sql::ExprKind::integer && integer && !type.is_unsigned()) type.length = static_cast<uint32_t>(type.precision) + 1;
      if (is_temporal_type(type)) {
        type.collation = settings.result_collation;
        type.length *= settings.result_max_bytes;
      }
    } else if (!query.set) {
      if (integer && item && item->kind != sql::ExprKind::column && type.precision <= 9) {
        type.field_type = f::long_;
        type.length = static_cast<uint32_t>(type.precision) + (type.is_unsigned() ? 0u : 1u);
      }
      if (type.flags & c::num) type.flags = static_cast<uint16_t>(type.flags & ~c::binary);
      if (is_string_type(type)) type.decimals = 0;
      type = without_keys(type);
    }
    catalog::Column column;
    column.name = result.columns[i].name;
    column.type = readable_type(result.columns[i].type);
    column.nullable = !(type.flags & c::not_null);
    view.table.columns.push_back(std::move(column));
    view.types.push_back(type);
    view.ordered.push_back(type);
    view.refused.emplace_back();
  }
  for (auto& row : result.rows) view.rows.push_back(std::move(row));
  return view;
}

std::vector<std::vector<Value>> Session::Queries::query_rows(const sql::Select& query, const ColumnSource* outer) {
  const auto out = _session.nested_select(query, outer, false);
  std::vector<std::vector<Value>> rows;
  if (!out.result) return rows;
  for (const auto& row : out.result->rows) {
    std::vector<Value> values;
    for (size_t i = 0; i < row.size(); ++i) values.push_back(value_from_text(row[i], out.result->columns[i].type));
    rows.push_back(std::move(values));
  }
  return rows;
}

Outcome Session::select_from_table(const sql::Select& select) {
  if (select.from.size() > 1 || select.from.front().is_join()) return select_joined(select);
  const auto& from = select.from.front();
  // A derived table is its query's result, read as a view.
  const auto database = from.derived ? std::string() : database_of(from.name);
  // information_schema's views are built now, read with no transaction.
  std::optional<View> view;
  std::optional<catalog::Table> table;
  if (from.derived) {
    view = derived_view(from);
    table = view->table;
  } else if (is_information_schema(database)) {
    view = information_schema_view(from.name.back(), _catalog);
    table = view->table;
  } else {
    table = _catalog.table({database, from.name.back()});
  }
  if (!table) {
    if (!_catalog.database(database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
    throw SqlError(1146, "42S02", "Table '" + database + "." + from.name.back() + "' doesn't exist");
  }
  _view = view ? &*view : nullptr;
  struct ViewScope {
    Session& s;
    ~ViewScope() { s._view = nullptr; }
  } view_scope{*this};
  if (is_grouped(select)) return select_groups(select, &*table, database);
  auto settings = eval_settings();
  TableColumns columns(*table, database, from.alias, settings, _view, through_temporary_table(select));
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
    auto type = evaluator.type(*item.expr);
    if (item.expr->kind == sql::ExprKind::column) {
      // A column keeps the name it was written with, without its qualifier;
      // a view's, the view's own (observed).
      const auto i = *columns.index(item.expr->path);
      result.columns.push_back(columns.column(i, item.has_alias ? alias_name(item.alias) : _view ? table->columns[i].name : item.expr->path.back()));
      outputs.push_back({i, item.expr.get(), type});
    } else {
      if (_view && through_temporary_table(select) && mentions_column(*item.expr)) type = as_view_field(type);
      result.columns.push_back({column_name(item), type});
      outputs.push_back({std::nullopt, item.expr.get(), type});
    }
  }
  if (select.distinct) {
    // DISTINCT goes through a temporary table (observed), unless the list
    // holds a whole key: its columns are that table's fields, grouped by.
    std::vector<size_t> listed;
    for (const auto& o : outputs)
      if (o.column) listed.push_back(*o.column);
    if (!holds_a_key(*table, listed))
      for (size_t k = 0; k < outputs.size(); ++k) {
        auto& type = result.columns[k].type;
        if (outputs[k].column) {
          if (type.nullable()) type.flags = static_cast<uint16_t>(type.flags | group_flag);
        } else if (mentions_column(*outputs[k].expr)) {
          type = as_field(type);
        }
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
  // Under DISTINCT an ORDER BY expression that is not a select item may name
  // only columns the list holds; else 3065, naming the first other column
  // (observed on MySQL 8.4.13).
  const auto refuse_unlisted = [&](const sql::Expr& e, size_t position) {
    for (const auto& o : outputs)
      if (o.expr && (o.expr == &e || sql::to_string(*o.expr) == sql::to_string(e))) return;
    const auto listed = [&](size_t i) { return std::any_of(outputs.begin(), outputs.end(), [&](const Output& o) { return o.column == i; }); };
    std::function<const sql::Expr*(const sql::Expr&)> stray = [&](const sql::Expr& x) -> const sql::Expr* {
      if (x.kind == sql::ExprKind::column) {
        const auto i = columns.index(x.path);
        return i && !listed(*i) ? &x : nullptr;
      }
      for (const auto& a : x.args)
        if (a)
          if (const auto* s = stray(*a)) return s;
      if (x.operand)
        if (const auto* s = stray(*x.operand)) return s;
      return x.otherwise ? stray(*x.otherwise) : nullptr;
    };
    if (const auto* s = stray(e))
      throw SqlError(3065, "HY000",
                     "Expression #" + std::to_string(position) + " of ORDER BY clause is not in SELECT list, references column '" + database + "." +
                         columns.qualifier() + "." + table->columns[*columns.index(s->path)].name +
                         "' which is not in SELECT list; this is incompatible with DISTINCT");
  };
  // A position counts the columns a star gives (observed); one of those is
  // sorted by as its column named.
  std::vector<std::unique_ptr<sql::Expr>> star_columns;
  for (const auto& order : select.order) {
    const sql::Expr* e = order.expr.get();
    if (is_position(*e)) {
      const uint64_t position = limit_of(*e);
      if (position < 1 || position > outputs.size()) throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + e->value + "' in 'order clause'");
      const auto& o = outputs[position - 1];
      if (!o.expr) {
        auto named = std::make_unique<sql::Expr>();
        named->kind = sql::ExprKind::column;
        named->path = {table->columns[*o.column].name};
        star_columns.push_back(std::move(named));
      }
      e = o.expr ? o.expr : star_columns.back().get();
    } else if (e->kind == sql::ExprKind::column && e->path.size() == 1) {
      // The first alias that names it; `e` is then that item's expression,
      // which may have no path to compare (JDBC's getTables: TABLE_TYPE is a
      // CASE's alias and a view's column).
      for (const auto& item : select.items)
        if (item.has_alias && sql::same_word(item.alias, e->path[0])) {
          e = item.expr.get();
          break;
        }
    }
    Sort sort;
    sort.expr = e;
    sort.descending = order.descending;
    in_clause("order clause", [&] { sort.type = evaluator.type(*e); });
    if (select.distinct) refuse_unlisted(*e, sorts.size() + 1);
    sort.collation = order_collation(*e, sort.type, columns, settings.result_collation);
    sorts.push_back(sort);
  }
  if (_describing) return described(std::move(result));
  const uint64_t limit = select.limit ? limit_of(*select.limit) : _variables.number("sql_select_limit");
  const uint64_t offset = select.offset ? limit_of(*select.offset) : 0;

  txn::Transaction* t = _view ? nullptr : &transaction();
  const auto* clause = _view ? nullptr : locking_clause(select, columns.qualifier());
  try {
    if (t) begin_reading(*t);
    struct Chosen {
      const txn::Row* row;
      std::vector<Value> keys;
    };
    std::vector<Chosen> chosen;
    // WHERE as the evaluator reads a condition (a string that is not wholly a
    // number warns 1292); one that names no column is read once, as MySQL
    // folds it (observed: WHERE 'x' over three rows warns once).
    const auto where_type = select.where ? evaluator.type(*select.where) : ResultType{};
    const auto passes = [&] {
      const auto b = evaluator.truth(evaluator.value(*select.where), where_type);
      return b && *b;
    };
    std::optional<bool> constant_where;
    if (select.where && !mentions_column(*select.where)) constant_where = passes();
    std::vector<txn::Row> rows;
    if (t && clause) {
      // A locking read: the current rows, those WHERE keeps locked.
      if (!_insert_source) _rows.begin_statement(*t);
      rows = locked_rows(*clause, *t, *table, key_range(select.where.get(), *table, columns, evaluator), [&](const txn::Row& r) {
        columns.set_row(&r);
        return !select.where || (constant_where ? *constant_where : passes());
      });
      columns.set_row(nullptr);
    } else {
      rows = t ? _rows.rows(*t, table->id) : _view->rows;
    }
    for (const auto& row : rows) {
      columns.set_row(&row);
      if (select.where && !(constant_where ? *constant_where : passes())) continue;
      Chosen c{&row, {}};
      for (const auto& sort : sorts) c.keys.push_back(evaluator.value(*sort.expr));
      chosen.push_back(std::move(c));
    }
    if (select.distinct) {
      // Each row once, the first met kept.
      std::vector<uint16_t> collations;
      for (const auto& o : outputs)
        collations.push_back(o.column ? columns.collation_at(*o.column) : key_collation(*o.expr, o.type, &columns, settings.result_collation));
      std::set<std::vector<Value>, ValuesLess> seen(ValuesLess{&collations});
      std::vector<Chosen> kept;
      for (auto& c : chosen) {
        columns.set_row(c.row);
        std::vector<Value> values;
        for (const auto& o : outputs) values.push_back(o.column ? columns.value_at(*o.column) : evaluator.value(*o.expr));
        if (seen.insert(std::move(values)).second) kept.push_back(std::move(c));
      }
      chosen = std::move(kept);
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
    columns.set_row(nullptr);
    // A locking read that could not finish gives back what it locked (an
    // INSERT's source, with the INSERT).
    if (t && clause && !_insert_source) _rows.undo_statement(*t);
    if (t) end_statement(true);
    throw;
  }
  if (t) end_statement(false);
  _warnings = evaluator.warnings();
  Outcome out;
  out.result = std::move(result);
  return out;
}

namespace {

// A join of the statement's tree over the leaves [first, end): its ON read
// over its own tables only. A FROM clause's comma list is a chain of these
// with no `ref`.
struct JoinNode {
  const sql::TableRef* ref = nullptr;
  sql::TableRef::Join join = sql::TableRef::Join::comma;
  size_t first = 0;
  size_t end = 0;
  std::unique_ptr<JoinNode> left;
  std::unique_ptr<JoinNode> right;
  std::unique_ptr<JoinedColumns> scope;
  std::unique_ptr<Evaluator> on;
  ResultType on_type;
  // USING's (or NATURAL's) columns: the left side's and the right side's,
  // each to equal the other.
  std::vector<std::pair<JoinPlace, JoinPlace>> using_pairs;
  // What `*` gives over this join, in order: USING's merged columns first,
  // then each side's others (the right side's first for RIGHT JOIN).
  std::vector<JoinPlace> star;
  // The tables in the order the plan reads them: a RIGHT JOIN's right side
  // first.
  std::vector<size_t> plan;
};

// A joined row: one row of each table, by leaf.
using Joined = std::vector<const txn::Row*>;

// What a temporary table reports a table's column as: its key flags gone.
ResultType without_keys(ResultType t) {
  t.flags = static_cast<uint16_t>(t.flags & ~(c::primary_key | c::unique_key | c::multiple_key | c::part_key));
  return t;
}

}  // namespace

// A FROM clause of joined tables: the tables, how USING merged their
// columns, the join tree, and the columns over all of them.
struct JoinFrom {
  JoinLeaves leaves;
  JoinLinks links;
  std::unique_ptr<JoinNode> root;
  std::unique_ptr<JoinedColumns> columns;

  // The joined row at hand (none: every table's row cleared).
  void set(const Joined* joined) {
    for (size_t l = 0; l < leaves.size(); ++l) leaves[l]->columns->set_row(joined ? (*joined)[l] : nullptr);
  }

  // The joined rows, over each table's rows as read: a row of the preserved
  // side with each of the other's its ON or USING keeps, and for an outer
  // join with the other side NULL when none does.
  std::vector<Joined> rows() { return rows(*root); }

  // What the ONs warned.
  std::vector<Warning> warnings() const {
    std::vector<Warning> out;
    std::function<void(const JoinNode&)> gather = [&](const JoinNode& node) {
      if (!node.left) return;
      gather(*node.left);
      gather(*node.right);
      if (node.on) out.insert(out.end(), node.on->warnings().begin(), node.on->warnings().end());
    };
    gather(*root);
    return out;
  }

 private:
  std::vector<Joined> rows(const JoinNode& node) {
    using Join = sql::TableRef::Join;
    std::vector<Joined> out;
    if (!node.left) {
      for (const auto& row : leaves[node.first]->rows) {
        Joined j(leaves.size(), nullptr);
        j[node.first] = &row;
        out.push_back(std::move(j));
      }
      return out;
    }
    const auto lefts = rows(*node.left);
    const auto rights = rows(*node.right);
    const auto keeps = [&](const Joined& j) {
      if (!node.on && node.using_pairs.empty()) return true;
      set(&j);
      // USING's columns equal, as `=` finds them: NULL equals nothing.
      for (const auto& [l, r] : node.using_pairs) {
        const auto a = leaves[l.leaf]->columns->value_at(l.column), b = leaves[r.leaf]->columns->value_at(r.column);
        if (is_null(a) || is_null(b) || compare_for_order(a, b, leaves[l.leaf]->columns->collation_at(l.column)) != 0) return false;
      }
      if (!node.on) return true;
      const auto b = node.on->truth(node.on->value(*node.ref->on), node.on_type);
      return b && *b;
    };
    const auto joined = [&](const Joined& l, const Joined& r) {
      Joined j = l;
      for (size_t k = node.right->first; k < node.right->end; ++k) j[k] = r[k];
      return j;
    };
    // An outer join walks its preserved side, and keeps a row of it that
    // nothing matched with the other side NULL.
    if (node.join == Join::right) {
      for (const auto& r : rights) {
        bool matched = false;
        for (const auto& l : lefts) {
          auto j = joined(l, r);
          if (!keeps(j)) continue;
          matched = true;
          out.push_back(std::move(j));
        }
        if (!matched) out.push_back(r);
      }
      return out;
    }
    for (const auto& l : lefts) {
      bool matched = false;
      for (const auto& r : rights) {
        auto j = joined(l, r);
        if (!keeps(j)) continue;
        matched = true;
        out.push_back(std::move(j));
      }
      if (!matched && node.join == Join::left) out.push_back(l);
    }
    return out;
  }
};

namespace {

using DatabaseOf = std::function<std::string(const std::vector<std::string>&)>;

// The joined tables of a FROM clause: each table found (1146, 1049), named
// once (1066); USING's and NATURAL's columns paired and merged (1054 'from
// clause'); each ON typed over its own join's tables (1054, 1052 'on
// clause'); an outer join's inner side marked as possibly NULL.
std::unique_ptr<JoinFrom> join_from(const std::vector<sql::TableRef>& references, const catalog::Catalog& catalog, const DatabaseOf& database_of,
                                    const EvalSettings& settings, const std::function<View(const sql::TableRef&)>& derived) {
  using Join = sql::TableRef::Join;
  auto from = std::make_unique<JoinFrom>();
  auto& leaves = from->leaves;
  auto& links = from->links;
  std::function<void(JoinNode&)> join_sides;
  std::function<std::unique_ptr<JoinNode>(const sql::TableRef&)> build = [&](const sql::TableRef& ref) {
    auto node = std::make_unique<JoinNode>();
    node->ref = &ref;
    node->first = leaves.size();
    if (!ref.is_join()) {
      auto leaf = std::make_unique<JoinLeaf>();
      std::string database;
      if (ref.derived) {
        // A derived table: its query's result, read as a view.
        leaf->view = std::make_unique<View>(derived(ref));
        leaf->table = leaf->view->table;
        leaf->columns = std::make_unique<TableColumns>(leaf->table, database, ref.alias, settings, leaf->view.get(), false);
      } else {
        database = database_of(ref.name);
        if (is_information_schema(database))
          throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'joins over information_schema'");
        auto table = catalog.table({database, ref.name.back()});
        if (!table) {
          if (!catalog.database(database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
          throw SqlError(1146, "42S02", "Table '" + database + "." + ref.name.back() + "' doesn't exist");
        }
        leaf->table = std::move(*table);
        leaf->columns = std::make_unique<TableColumns>(leaf->table, database, ref.alias, settings);
      }
      leaf->database = database;
      // A table or alias named twice (observed: by its alias, or its name).
      for (const auto& other : leaves) {
        const bool both_bare = ref.alias.empty() && other->columns->qualifier() == other->table.name && other->database != database;
        if (other->columns->qualifier() == leaf->columns->qualifier() && !both_bare)
          throw SqlError(1066, "42000", "Not unique table/alias: '" + leaf->columns->qualifier() + "'");
      }
      for (size_t i = 0; i < leaf->table.columns.size(); ++i) node->star.push_back({leaves.size(), i});
      node->plan = {leaves.size()};
      leaves.push_back(std::move(leaf));
      node->end = leaves.size();
      return node;
    }
    node->join = ref.join;
    node->left = build(*ref.left);
    node->right = build(*ref.right);
    node->end = leaves.size();
    join_sides(*node);
    return node;
  };
  // A join's sides joined: USING's or NATURAL's columns paired and merged,
  // its `*`, and its plan.
  join_sides = [&](JoinNode& node) {
    const bool right_join = node.join == Join::right;
    const auto& first = right_join ? *node.right : *node.left;
    const auto& second = right_join ? *node.left : *node.right;
    node.plan = first.plan;
    node.plan.insert(node.plan.end(), second.plan.begin(), second.plan.end());
    std::vector<std::string> names;
    if (node.ref && node.ref->natural) {
      // Every column the two sides' `*` share, in the left side's order.
      for (const auto& l : node.left->star) {
        const auto& name = leaves[l.leaf]->table.columns[l.column].name;
        const bool shared = std::any_of(node.right->star.begin(), node.right->star.end(),
                                        [&](const JoinPlace& r) { return sql::same_word(leaves[r.leaf]->table.columns[r.column].name, name); });
        if (shared && std::none_of(names.begin(), names.end(), [&](const std::string& n) { return sql::same_word(n, name); })) names.push_back(name);
      }
    } else if (node.ref && node.ref->has_using) {
      names = node.ref->using_columns;
    }
    std::vector<JoinPlace> merged;
    for (const auto& name : names) {
      const auto side = [&](const JoinNode& n) {
        const JoinedColumns scope(leaves, links, n.first, n.end);
        std::optional<JoinPlace> p;
        in_clause("from clause", [&] { p = scope.place({name}); });
        if (!p) throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + name + "' in 'from clause'");
        return *p;
      };
      const auto l = side(*node.left), r = side(*node.right);
      node.using_pairs.emplace_back(l, r);
      // The preserved side's column stands for both: the left's, but a
      // RIGHT JOIN's right's.
      const auto kept = right_join ? r : l;
      links[right_join ? l : r] = JoinLink{kept, node.first, node.end};
      merged.push_back(kept);
    }
    const JoinedColumns scope(leaves, links, node.first, node.end);
    node.star = merged;
    for (const auto* side : {&first, &second})
      for (const auto& p : side->star)
        if (std::find(merged.begin(), merged.end(), scope.merged(p)) == merged.end()) node.star.push_back(p);
  };
  for (const auto& ref : references) {
    auto next = build(ref);
    if (!from->root) {
      from->root = std::move(next);
      continue;
    }
    auto comma = std::make_unique<JoinNode>();
    comma->first = from->root->first;
    comma->end = next->end;
    comma->left = std::move(from->root);
    comma->right = std::move(next);
    join_sides(*comma);
    from->root = std::move(comma);
  }
  // An outer join's inner side may come back NULL: a LEFT JOIN's right, a
  // RIGHT JOIN's left, and everything inside them.
  std::function<void(const JoinNode&, bool)> mark = [&](const JoinNode& node, bool nullable) {
    if (!node.left) {
      leaves[node.first]->nullable = nullable;
      return;
    }
    mark(*node.left, nullable || node.join == Join::right);
    mark(*node.right, nullable || node.join == Join::left);
  };
  mark(*from->root, false);
  // Each ON, typed over its own join's tables.
  std::function<void(JoinNode&)> type_on = [&](JoinNode& node) {
    if (!node.left) return;
    type_on(*node.left);
    type_on(*node.right);
    if (!node.ref || !node.ref->on) return;
    node.scope = std::make_unique<JoinedColumns>(leaves, links, node.first, node.end);
    auto scoped = settings;
    scoped.columns = node.scope.get();
    node.on = std::make_unique<Evaluator>(scoped);
    in_clause("on clause", [&] { node.on_type = node.on->type(*node.ref->on); });
  };
  type_on(*from->root);
  from->columns = std::make_unique<JoinedColumns>(leaves, links, 0, leaves.size());
  return from;
}

// The terms of a condition joined by AND.
void conjuncts(const sql::Expr& e, std::vector<const sql::Expr*>& out) {
  if (e.kind == sql::ExprKind::binary && e.value == "AND") {
    conjuncts(*e.args[0], out);
    conjuncts(*e.args[1], out);
    return;
  }
  out.push_back(&e);
}

// The tables an expression reads, or nothing when a name in it cannot be
// placed (it is reported where it is typed).
std::optional<std::set<size_t>> tables_of(const sql::Expr& e, const JoinedColumns& columns) {
  std::set<size_t> out;
  bool placed = true;
  std::function<void(const sql::Expr&)> walk = [&](const sql::Expr& x) {
    if (x.kind == sql::ExprKind::column) {
      try {
        if (const auto p = columns.place(x.path))
          out.insert(p->leaf);
        else
          placed = false;
      } catch (const SqlError&) {
        placed = false;
      }
      return;
    }
    for (const auto& a : x.args)
      if (a) walk(*a);
    if (x.operand) walk(*x.operand);
    if (x.otherwise) walk(*x.otherwise);
  };
  walk(e);
  if (!placed) return std::nullopt;
  return out;
}

// The column a bare or qualified name places, or nothing.
std::optional<JoinPlace> placed(const sql::Expr& column, const JoinedColumns& columns) {
  if (column.kind != sql::ExprKind::column) return std::nullopt;
  try {
    return columns.place(column.path);
  } catch (const SqlError&) {
    return std::nullopt;
  }
}

// The equalities a join's tables meet: every ON's terms and USING's pairs,
// and WHERE's terms (marked, as they say nothing of an outer join's inner
// side).
struct JoinTerms {
  std::vector<std::pair<const sql::Expr*, bool>> terms;  // a term, from WHERE
  std::vector<std::pair<JoinPlace, JoinPlace>> pairs;
};

JoinTerms join_terms(const sql::Select& select, const JoinNode& root) {
  JoinTerms out;
  std::function<void(const JoinNode&)> collect = [&](const JoinNode& node) {
    if (!node.left) return;
    collect(*node.left);
    collect(*node.right);
    if (node.ref && node.ref->on) {
      std::vector<const sql::Expr*> on;
      conjuncts(*node.ref->on, on);
      for (const auto* term : on) out.terms.emplace_back(term, false);
    }
    out.pairs.insert(out.pairs.end(), node.using_pairs.begin(), node.using_pairs.end());
  };
  collect(root);
  if (select.where) {
    std::vector<const sql::Expr*> where;
    conjuncts(*select.where, where);
    for (const auto* term : where) out.terms.emplace_back(term, true);
  }
  return out;
}

// Whether the fixed plan finds each table after the first through an index:
// by an equality between the leading column of one of its keys and what the
// tables before it hold (a constant, or their columns). A table read whole
// goes through a join buffer (observed on MySQL 8.4.13).
bool found_through_indexes(const sql::Select& select, const JoinNode& root, const JoinLeaves& leaves, const JoinedColumns& columns) {
  const auto& plan = root.plan;
  std::vector<size_t> position(leaves.size());
  for (size_t k = 0; k < plan.size(); ++k) position[plan[k]] = k;
  const auto [terms, pairs] = join_terms(select, root);
  const auto leads = [&](JoinPlace p) {
    for (const auto& key : leaves[p.leaf]->table.keys)
      if (!key.parts.empty() && key.parts[0].column == p.column && !key.parts[0].prefix) return true;
    return false;
  };
  for (size_t k = 1; k < plan.size(); ++k) {
    const size_t table = plan[k];
    const auto before = [&](const std::set<size_t>& tables) { return std::all_of(tables.begin(), tables.end(), [&](size_t t) { return position[t] < k; }); };
    bool found = false;
    for (const auto& [l, r] : pairs)
      found = found || (l.leaf == table && leads(l) && position[r.leaf] < k) || (r.leaf == table && leads(r) && position[l.leaf] < k);
    for (const auto& [term, from_where] : terms) {
      if (found) break;
      if (from_where && leaves[table]->nullable) continue;
      if (term->kind != sql::ExprKind::binary || term->value != "=" || term->args.size() != 2) continue;
      for (size_t side = 0; side < 2 && !found; ++side) {
        const auto p = placed(*term->args[side], columns);
        if (!p || p->leaf != table || !leads(*p)) continue;
        const auto other = tables_of(*term->args[1 - side], columns);
        found = other && before(*other);
      }
    }
    if (!found) return false;
  }
  return true;
}

// Whether the fixed plan sorts a join through a temporary table (observed
// on MySQL 8.4.13): it does unless every sort key is the first table's and
// each later table is found through an index; else the joined rows are
// buffered and sorted apart.
bool sorts_through_temporary(const sql::Select& select, const JoinNode& root, const JoinLeaves& leaves, const JoinedColumns& columns,
                             const std::vector<std::vector<JoinPlace>>& stars) {
  const auto& plan = root.plan;
  for (const auto& order : select.order) {
    const sql::Expr* e = order.expr.get();
    std::optional<JoinPlace> star_column;
    if (is_position(*e)) {
      const uint64_t n = integer_text(*e);
      size_t at = 0;
      e = nullptr;
      for (size_t k = 0; k < select.items.size() && !e && !star_column; ++k) {
        if (!select.items[k].star) {
          if (++at == n) e = select.items[k].expr.get();
          continue;
        }
        if (n > at && n <= at + stars[k].size()) star_column = stars[k][n - at - 1];
        at += stars[k].size();
      }
      if (!e && !star_column) return true;
    } else if (e->kind == sql::ExprKind::column && e->path.size() == 1) {
      for (const auto& item : select.items)
        if (item.has_alias && sql::same_word(item.alias, e->path[0])) {
          e = item.expr.get();
          break;
        }
    }
    if (star_column) {
      if (star_column->leaf != plan.front()) return true;
      continue;
    }
    const auto tables = tables_of(*e, columns);
    if (!tables) return true;
    for (const auto t : *tables)
      if (t != plan.front()) return true;
  }
  return !found_through_indexes(select, root, leaves, columns);
}

// A join's columns as grouping reads them, each numbered: a table's
// columns follow the tables before it's.
class JoinIndexed : public IndexedColumns {
 public:
  JoinIndexed(const JoinFrom& from, const sql::Select& select) : _from(from), _select(select) {
    size_t at = 0;
    for (const auto& leaf : from.leaves) {
      _offsets.push_back(at);
      at += leaf->table.columns.size();
    }
    // Two columns an equality between them makes one, for what follows from
    // what: ON's and WHERE's `column = column`, and USING's pairs.
    const auto [terms, pairs] = join_terms(select, *from.root);
    for (const auto& [l, r] : pairs) _equal.emplace_back(flat(l), flat(r));
    for (const auto& [term, from_where] : terms) {
      if (term->kind != sql::ExprKind::binary || term->value != "=" || term->args.size() != 2) continue;
      const auto l = placed(*term->args[0], *from.columns), r = placed(*term->args[1], *from.columns);
      if (l && r) _equal.emplace_back(flat(*l), flat(*r));
    }
  }

  std::optional<Found> find_column(const std::vector<std::string>& path) const override { return _from.columns->find_column(path); }
  Value column_value(const std::vector<std::string>& path) const override { return _from.columns->column_value(path); }
  std::string written(const std::vector<std::string>& path) const override { return _from.columns->written(path); }

  std::optional<size_t> index(const std::vector<std::string>& path) const override {
    const auto p = _from.columns->place(path);
    if (!p) return std::nullopt;
    return flat(*p);
  }
  Column column(size_t i, std::string name) const override {
    const auto p = place(i);
    auto out = leaf(i).columns->column(p.column, std::move(name));
    if (leaf(i).nullable) out.type.flags = static_cast<uint16_t>(out.type.flags & ~c::not_null);
    return out;
  }
  Value value_at(size_t i) const override { return leaf(i).columns->value_at(place(i).column); }
  std::optional<std::string> text(size_t i) const override { return leaf(i).columns->text(place(i).column); }
  uint16_t collation_at(size_t i) const override { return leaf(i).columns->collation_at(place(i).column); }
  const std::string& name_of(size_t i) const override { return leaf(i).table.columns[place(i).column].name; }
  std::string named(size_t i) const override { return leaf(i).columns->named(place(i).column); }

  std::vector<size_t> star(const std::vector<std::string>& p) const override {
    std::vector<size_t> out;
    if (p.empty()) {
      for (const auto& at : _from.root->star) out.push_back(flat(at));
      return out;
    }
    for (size_t l = 0; l < _from.leaves.size(); ++l) {
      const auto& leaf = *_from.leaves[l];
      const bool ours = (p.size() == 1 && p[0] == leaf.columns->qualifier()) ||
                        (p.size() == 2 && leaf.columns->qualifier() == leaf.table.name && p[0] == leaf.database && p[1] == leaf.table.name);
      if (ours)
        for (size_t i = 0; i < leaf.table.columns.size(); ++i) out.push_back(_offsets[l] + i);
    }
    if (out.empty()) throw SqlError(1051, "42S02", "Unknown table '" + p.back() + "'");
    return out;
  }

  // What follows from known columns, to a fixed point: a column equal to a
  // known one is known; a table one of whose keys is known is known whole.
  std::vector<size_t> determined(std::vector<size_t> known) const override {
    const auto has = [&](size_t i) { return std::find(known.begin(), known.end(), i) != known.end(); };
    for (bool more = true; more;) {
      more = false;
      for (const auto& [a, b] : _equal) {
        if (has(a) && !has(b)) known.push_back(b), more = true;
        if (has(b) && !has(a)) known.push_back(a), more = true;
      }
      for (size_t l = 0; l < _from.leaves.size(); ++l) {
        const auto& table = _from.leaves[l]->table;
        std::vector<size_t> own;
        for (size_t i = 0; i < table.columns.size(); ++i)
          if (has(_offsets[l] + i)) own.push_back(i);
        if (own.size() == table.columns.size() || !holds_a_key(table, own)) continue;
        for (size_t i = 0; i < table.columns.size(); ++i)
          if (!has(_offsets[l] + i)) known.push_back(_offsets[l] + i), more = true;
      }
    }
    return known;
  }

  // Off an index only when the plan's first table is grouped by one of its
  // indexes and each table after it is found through an index (observed).
  bool grouped_by_index(const std::vector<size_t>& by) const override {
    const size_t first = _from.root->plan.front();
    std::vector<size_t> own;
    for (const auto i : by) {
      if (place(i).leaf != first) return false;
      own.push_back(place(i).column);
    }
    return _from.leaves[first]->columns->grouped_by_index(own) && found_through_indexes(_select, *_from.root, _from.leaves, *_from.columns);
  }

 private:
  size_t flat(JoinPlace p) const { return _offsets[p.leaf] + p.column; }
  JoinPlace place(size_t i) const {
    size_t l = _offsets.size() - 1;
    while (_offsets[l] > i) --l;
    return {l, i - _offsets[l]};
  }
  const JoinLeaf& leaf(size_t i) const { return *_from.leaves[place(i).leaf]; }

  const JoinFrom& _from;
  const sql::Select& _select;
  std::vector<size_t> _offsets;
  std::vector<std::pair<size_t, size_t>> _equal;
};

}  // namespace

bool Session::read_join(JoinFrom& from, const std::vector<const sql::Select::Locking*>& clauses, txn::Transaction& t) {
  begin_reading(t);
  const bool locking = std::any_of(clauses.begin(), clauses.end(), [](const auto* c) { return c != nullptr; });
  if (locking && !_insert_source) _rows.begin_statement(t);
  for (size_t l = 0; l < from.leaves.size(); ++l) {
    auto& leaf = *from.leaves[l];
    leaf.rows.clear();
    leaf.keys.clear();
    const auto* clause = clauses[l];
    if (leaf.view) {
      leaf.rows = leaf.view->rows;
      continue;
    }
    if (!clause) {
      leaf.rows = _rows.rows(t, leaf.table.id);
      continue;
    }
    const auto mode = clause->exclusive ? txn::Store::LockMode::exclusive : txn::Store::LockMode::shared;
    for (auto& held : _rows.current(t, leaf.table.id)) {
      if (clause->wait == sql::Select::Locking::Wait::skip_locked && _rows.held_by_other(t, leaf.table.id, held.key, mode)) continue;
      leaf.keys.push_back(std::move(held.key));
      leaf.rows.push_back(std::move(held.row));
    }
  }
  return locking;
}

void Session::lock_join(JoinFrom& from, const std::vector<const sql::Select::Locking*>& clauses, txn::Transaction& t,
                        const std::vector<const std::vector<const txn::Row*>*>& kept, const sql::Expr* where) {
  using Wait = sql::Select::Locking::Wait;
  const auto level = _variables.text("transaction_isolation");
  const bool gaps = !sql::same_word(level, "READ-COMMITTED") && !sql::same_word(level, "READ-UNCOMMITTED");
  for (size_t l = 0; l < from.leaves.size(); ++l) {
    const auto* clause = clauses[l];
    if (!clause) continue;
    auto& leaf = *from.leaves[l];
    const auto blocked = [&](uint64_t holder) {
      if (clause->wait == Wait::nowait) throw SqlError(3572, "HY000", "Statement aborted because lock(s) could not be acquired immediately and NOWAIT is set.");
      throw LockWaitNeeded(t.id(), holder);
    };
    // The rows of this table the kept joined rows hold (none for a row an
    // outer join made NULL).
    std::vector<std::string> keys;
    for (const auto* joined : kept) {
      const auto* row = (*joined)[l];
      if (!row) continue;
      const auto& key = leaf.keys[static_cast<size_t>(row - leaf.rows.data())];
      if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    }
    uint64_t holder = 0;
    if (gaps && clause->wait != Wait::skip_locked) {
      auto settings = eval_settings();
      settings.columns = leaf.columns.get();
      Evaluator evaluator(settings);
      if (!_rows.lock_range(t, leaf.table.id, key_range(where, leaf.table, *leaf.columns, evaluator), &holder)) blocked(holder);
    }
    const auto mode = clause->exclusive ? txn::Store::LockMode::exclusive : txn::Store::LockMode::shared;
    if (!_rows.lock_rows(t, leaf.table.id, keys, mode, &holder)) blocked(holder);
  }
}

// Joins on the fixed plan (the maintainer's ruling of 2026-11-23): each
// join's tables in written order (a RIGHT JOIN's right side first), nested
// loops, a row of the preserved side with each of the other's that its ON
// or USING keeps, and with the other side NULL for an outer join when none
// does. Names resolve across the tables in scope (an ON's are its own
// join's; USING's merged columns answer to their bare names). A join made
// DISTINCT, or sorted other than as sorts_through_temporary allows, goes
// through a temporary table, which drops its columns' key flags and reads
// its expressions as fields (observed on MySQL 8.4.13, where the
// optimizer's order decides; recorded where it differs).
Outcome Session::select_joined(const sql::Select& select) {
  auto settings = eval_settings();
  auto from = join_from(
      select.from, _catalog, [&](const std::vector<std::string>& name) { return database_of(name); }, settings,
      [&](const sql::TableRef& ref) { return derived_view(ref); });
  if (is_grouped(select)) return select_groups(select, nullptr, "", from.get());
  auto& leaves = from->leaves;
  const auto& root = from->root;
  const JoinedColumns& columns = *from->columns;
  settings.columns = &columns;
  Evaluator evaluator(settings);
  const auto set_joined = [&](const Joined* joined) { from->set(joined); };
  using Place = JoinedColumns::Place;
  // Each star item's columns: `*` the join's, `t.*` the table's own.
  std::vector<std::vector<Place>> stars(select.items.size());
  for (size_t k = 0; k < select.items.size(); ++k) {
    const auto& item = select.items[k];
    if (!item.star) continue;
    const auto& p = item.star_path;
    if (p.empty()) {
      stars[k] = root->star;
      continue;
    }
    for (size_t l = 0; l < leaves.size(); ++l) {
      const auto& leaf = *leaves[l];
      const bool ours = (p.size() == 1 && p[0] == leaf.columns->qualifier()) ||
                        (p.size() == 2 && leaf.columns->qualifier() == leaf.table.name && p[0] == leaf.database && p[1] == leaf.table.name);
      if (ours)
        for (size_t i = 0; i < leaf.table.columns.size(); ++i) stars[k].push_back({l, i});
    }
    if (stars[k].empty()) throw SqlError(1051, "42S02", "Unknown table '" + p.back() + "'");
  }
  const bool temporary = select.distinct || (!select.order.empty() && sorts_through_temporary(select, *root, leaves, columns, stars));
  ResultSet result;
  struct Output {
    std::optional<Place> column;
    const sql::Expr* expr = nullptr;
    ResultType type;
  };
  std::vector<Output> outputs;
  const auto add_column = [&](Place p, std::string name, const sql::Expr* expr) {
    auto column = leaves[p.leaf]->columns->column(p.column, std::move(name));
    if (leaves[p.leaf]->nullable) column.type.flags = static_cast<uint16_t>(column.type.flags & ~c::not_null);
    if (temporary) column.type = without_keys(column.type);
    if (select.distinct && column.type.nullable()) column.type.flags = static_cast<uint16_t>(column.type.flags | group_flag);
    outputs.push_back({p, expr, column.type});
    result.columns.push_back(std::move(column));
  };
  for (size_t k = 0; k < select.items.size(); ++k) {
    const auto& item = select.items[k];
    if (item.star) {
      for (const auto& p : stars[k]) add_column(p, leaves[p.leaf]->table.columns[p.column].name, nullptr);
      continue;
    }
    auto type = evaluator.type(*item.expr);
    if (item.expr->kind == sql::ExprKind::column) {
      add_column(*columns.place(item.expr->path), item.has_alias ? alias_name(item.alias) : item.expr->path.back(), item.expr.get());
    } else {
      if (temporary && mentions_column(*item.expr)) type = as_field(type);
      result.columns.push_back({column_name(item), type});
      outputs.push_back({std::nullopt, item.expr.get(), type});
    }
  }
  if (select.where) in_clause("where clause", [&] { evaluator.type(*select.where); });
  struct Sort {
    const sql::Expr* expr = nullptr;
    ResultType type;
    uint16_t collation = 0;
    bool descending = false;
  };
  std::vector<Sort> sorts;
  // Under DISTINCT an ORDER BY expression that is not a select item may name
  // only columns the list holds (3065, as for one table).
  const auto refuse_unlisted = [&](const sql::Expr& e, size_t position) {
    for (const auto& o : outputs)
      if (o.expr && (o.expr == &e || sql::to_string(*o.expr) == sql::to_string(e))) return;
    const auto listed = [&](Place p) {
      return std::any_of(outputs.begin(), outputs.end(), [&](const Output& o) { return o.column && o.column->leaf == p.leaf && o.column->column == p.column; });
    };
    std::function<std::optional<Place>(const sql::Expr&)> stray = [&](const sql::Expr& x) -> std::optional<Place> {
      if (x.kind == sql::ExprKind::column) {
        const auto p = columns.place(x.path);
        return p && !listed(*p) ? p : std::nullopt;
      }
      for (const auto& a : x.args)
        if (a)
          if (auto s = stray(*a)) return s;
      if (x.operand)
        if (auto s = stray(*x.operand)) return s;
      return x.otherwise ? stray(*x.otherwise) : std::nullopt;
    };
    if (const auto s = stray(e)) {
      const auto& leaf = *leaves[s->leaf];
      throw SqlError(3065, "HY000",
                     "Expression #" + std::to_string(position) + " of ORDER BY clause is not in SELECT list, references column '" + leaf.database + "." +
                         leaf.columns->qualifier() + "." + leaf.table.columns[s->column].name +
                         "' which is not in SELECT list; this is incompatible with DISTINCT");
    }
  };
  // A position names a select item, a star's column by its table.
  std::vector<std::unique_ptr<sql::Expr>> star_columns;
  for (const auto& order : select.order) {
    const sql::Expr* e = order.expr.get();
    if (is_position(*e)) {
      const uint64_t position = limit_of(*e);
      if (position < 1 || position > outputs.size()) throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + e->value + "' in 'order clause'");
      const auto& o = outputs[position - 1];
      if (!o.expr) {
        const auto& leaf = *leaves[o.column->leaf];
        auto named = std::make_unique<sql::Expr>();
        named->kind = sql::ExprKind::column;
        named->path = {leaf.columns->qualifier(), leaf.table.columns[o.column->column].name};
        if (leaf.columns->qualifier() == leaf.table.name) named->path.insert(named->path.begin(), leaf.database);
        star_columns.push_back(std::move(named));
      }
      e = o.expr ? o.expr : star_columns.back().get();
    } else if (e->kind == sql::ExprKind::column && e->path.size() == 1) {
      for (const auto& item : select.items)
        if (item.has_alias && sql::same_word(item.alias, e->path[0])) {
          e = item.expr.get();
          break;
        }
    }
    Sort sort;
    sort.expr = e;
    sort.descending = order.descending;
    in_clause("order clause", [&] { sort.type = evaluator.type(*e); });
    if (select.distinct) refuse_unlisted(*e, sorts.size() + 1);
    sort.collation = order_collation(*e, sort.type, columns, settings.result_collation);
    sorts.push_back(sort);
  }
  if (_describing) return described(std::move(result));
  const uint64_t limit = select.limit ? limit_of(*select.limit) : _variables.number("sql_select_limit");
  const uint64_t offset = select.offset ? limit_of(*select.offset) : 0;

  std::vector<std::string> qualifiers;
  for (const auto& leaf : leaves) qualifiers.push_back(leaf->columns->qualifier());
  const auto clauses = locking_clauses(select, qualifiers);
  auto& t = transaction();
  bool locking = false;
  try {
    locking = read_join(*from, clauses, t);
    const auto where_type = select.where ? evaluator.type(*select.where) : ResultType{};
    const auto passes = [&] {
      const auto b = evaluator.truth(evaluator.value(*select.where), where_type);
      return b && *b;
    };
    std::optional<bool> constant_where;
    if (select.where && !mentions_column(*select.where)) constant_where = passes();
    struct Chosen {
      Joined row;
      std::vector<Value> keys;
    };
    std::vector<Chosen> chosen;
    if (!constant_where || *constant_where)
      for (auto& j : from->rows()) {
        set_joined(&j);
        if (select.where && !constant_where && !passes()) continue;
        Chosen c{std::move(j), {}};
        for (const auto& sort : sorts) c.keys.push_back(evaluator.value(*sort.expr));
        chosen.push_back(std::move(c));
      }
    // A locking read locks the rows WHERE kept, each table's by its clause.
    if (locking) {
      std::vector<const Joined*> kept;
      for (const auto& c : chosen) kept.push_back(&c.row);
      lock_join(*from, clauses, t, kept, select.where.get());
    }
    const auto value_of_output = [&](const Output& o) {
      return o.column ? leaves[o.column->leaf]->columns->value_at(o.column->column) : evaluator.value(*o.expr);
    };
    if (select.distinct) {
      std::vector<uint16_t> collations;
      for (const auto& o : outputs)
        collations.push_back(o.column ? leaves[o.column->leaf]->columns->collation_at(o.column->column)
                                      : key_collation(*o.expr, o.type, &columns, settings.result_collation));
      std::set<std::vector<Value>, ValuesLess> seen(ValuesLess{&collations});
      std::vector<Chosen> kept;
      for (auto& c : chosen) {
        set_joined(&c.row);
        std::vector<Value> values;
        for (const auto& o : outputs) values.push_back(value_of_output(o));
        if (seen.insert(std::move(values)).second) kept.push_back(std::move(c));
      }
      chosen = std::move(kept);
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
      set_joined(&chosen[i].row);
      std::vector<std::optional<std::string>> out;
      for (const auto& o : outputs)
        out.push_back(o.column ? leaves[o.column->leaf]->columns->text(o.column->column) : to_text(evaluator.value(*o.expr), o.type));
      result.rows.push_back(std::move(out));
    }
    set_joined(nullptr);
  } catch (...) {
    set_joined(nullptr);
    // A locking read that could not finish gives back what it locked (an
    // INSERT's source, with the INSERT).
    if (locking && !_insert_source) _rows.undo_statement(t);
    end_statement(true);
    throw;
  }
  end_statement(false);
  auto warnings = evaluator.warnings();
  const auto more = from->warnings();
  warnings.insert(warnings.end(), more.begin(), more.end());
  _warnings = std::move(warnings);
  Outcome out;
  out.result = std::move(result);
  return out;
}

// Grouping as MySQL 8.4 does it (the manual's "MySQL Handling of GROUP BY"
// and "Aggregate Function Descriptions", observed on 8.4.13): rows WHERE
// keeps are grouped by GROUP BY's values (NULL a group of its own), in the
// order the groups are first met; with aggregates and no GROUP BY they are
// one group, even of no rows; with neither, each row is its own. Each
// group's aggregates are taken over its rows, HAVING filters the groups,
// then ORDER BY and LIMIT. Under ONLY_FULL_GROUP_BY a column read outside
// an aggregate must be grouped, fixed by WHERE (column = constant), or
// follow from a grouped key (1055, 1140); without it, it is the group's
// first row's.
Outcome Session::select_groups(const sql::Select& select, const catalog::Table* table, const std::string& database, JoinFrom* join) {
  if (select.with_rollup) throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'WITH ROLLUP'");
  auto settings = eval_settings();
  std::optional<TableColumns> table_columns;
  std::optional<JoinIndexed> join_columns;
  const IndexedColumns* columns = nullptr;
  if (table) {
    table_columns.emplace(*table, database, select.from.front().alias, settings, _view, through_temporary_table(select));
    columns = &*table_columns;
  } else if (join) {
    join_columns.emplace(*join, select);
    columns = &*join_columns;
  }
  settings.columns = columns;
  // Reads each row: WHERE, the group's values, aggregates' arguments. An
  // aggregate here is 1111.
  Evaluator rows(settings);
  if (select.where) in_clause("where clause", [&] { rows.type(*select.where); });

  // The result's columns: a table's column, or an expression.
  struct Output {
    std::optional<size_t> column;
    const sql::Expr* expr = nullptr;
    std::string name;
    ResultType type;
  };
  std::vector<Output> outputs;
  for (const auto& item : select.items) {
    if (item.star) {
      if (!columns) throw SqlError(er_no_tables_used, "HY000", "No tables used");
      for (const auto i : columns->star(item.star_path)) outputs.push_back({i, nullptr, columns->name_of(i), {}});
      continue;
    }
    std::optional<size_t> column;
    if (item.expr->kind == sql::ExprKind::column && columns) column = columns->index(item.expr->path);
    const auto column_named = [&] { return item.has_alias ? alias_name(item.alias) : _view ? columns->name_of(*column) : item.expr->path.back(); };
    outputs.push_back({column, item.expr.get(), column ? column_named() : column_name(item), {}});
  }

  // GROUP BY: a table's column before an alias; a position.
  const auto is_table_column = [&](const sql::Expr& x) {
    std::optional<size_t> i;
    in_clause("group statement", [&] { i = columns->index(x.path); });
    return i.has_value();
  };
  std::vector<const sql::Expr*> groups_by;
  for (const auto& g : select.group_by) {
    const sql::Expr* e = g.get();
    if (is_position(*e)) {
      const uint64_t position = limit_of(*e);
      if (position < 1 || position > select.items.size() || select.items[position - 1].star)
        throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + e->value + "' in 'group statement'");
      e = select.items[position - 1].expr.get();
    } else if (e->kind == sql::ExprKind::column && e->path.size() == 1 && !(columns && is_table_column(*e))) {
      for (const auto& item : select.items)
        if (item.has_alias && sql::same_word(item.alias, e->path[0])) {
          e = item.expr.get();
          break;
        }
    }
    if (contains_aggregate(*e)) throw SqlError(1056, "42000", "Can't group on '" + written(*e) + "'");
    in_clause("group statement", [&] { rows.type(*e); });
    groups_by.push_back(e);
  }

  // ORDER BY: a select item by its position or its alias, else an
  // expression over the group.
  std::vector<const sql::Expr*> order_exprs;
  for (const auto& order : select.order) {
    const sql::Expr* e = order.expr.get();
    if (is_position(*e)) {
      const uint64_t position = limit_of(*e);
      if (position < 1 || position > select.items.size() || select.items[position - 1].star)
        throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + e->value + "' in 'order clause'");
      e = select.items[position - 1].expr.get();
    } else if (e->kind == sql::ExprKind::column && e->path.size() == 1) {
      for (const auto& item : select.items)
        if (item.has_alias && sql::same_word(item.alias, e->path[0])) {
          e = item.expr.get();
          break;
        }
    }
    order_exprs.push_back(e);
  }

  // The aggregate calls, each typed over the rows.
  std::vector<const sql::Expr*> calls;
  for (const auto& o : outputs)
    if (o.expr) collect_aggregates(*o.expr, calls);
  if (select.having) collect_aggregates(*select.having, calls);
  for (const auto* e : order_exprs) collect_aggregates(*e, calls);
  std::vector<AggregateSpec> specs;
  specs.reserve(calls.size());
  for (const auto* call : calls) {
    std::vector<ResultType> types;
    std::vector<uint16_t> collations;
    for (const auto& arg : call->args) {
      types.push_back(rows.type(*arg));
      collations.push_back(key_collation(*arg, types.back(), columns, settings.result_collation));
    }
    const bool of_column = call->args.size() == 1 && call->args[0]->kind == sql::ExprKind::column;
    specs.emplace_back(*call, std::move(types), std::move(collations), of_column, settings);
  }

  // Reads each group: the select list, ORDER BY, and through an alias,
  // HAVING.
  GroupAggregates aggregates(specs);
  auto group_settings = settings;
  group_settings.aggregates = &aggregates;
  Evaluator items(group_settings);
  for (auto& o : outputs) o.type = o.column ? columns->column(*o.column, o.name).type : items.type(*o.expr);
  HavingColumns having_columns(columns, select, groups_by, items, settings.result_collation);
  auto having_settings = group_settings;
  having_settings.columns = &having_columns;
  Evaluator having(having_settings);
  if (select.having) in_clause("having clause", [&] { having.type(*select.having); });
  struct Sort {
    const sql::Expr* expr = nullptr;
    uint16_t collation = 0;
    bool descending = false;
  };
  std::vector<Sort> sorts;
  for (size_t i = 0; i < order_exprs.size(); ++i) {
    ResultType type;
    in_clause("order clause", [&] { type = items.type(*order_exprs[i]); });
    sorts.push_back({order_exprs[i], key_collation(*order_exprs[i], type, columns, settings.result_collation), select.order[i].descending});
  }

  const bool implicit = groups_by.empty() && !specs.empty();
  const bool per_row = groups_by.empty() && specs.empty();
  std::vector<size_t> fixed, grouped;
  if (columns) {
    if (select.where) fixed_columns(*select.where, *columns, fixed);
    for (const auto* g : groups_by)
      if (g->kind == sql::ExprKind::column) grouped.push_back(*columns->index(g->path));
  }
  const auto has = [](const std::vector<size_t>& v, size_t i) { return std::find(v.begin(), v.end(), i) != v.end(); };
  // Grouped by columns that lead an index, MySQL reads the groups off the
  // index with no temporary table (observed: expressions keep BINARY).
  const auto grouped_by_index = [&](const std::vector<const sql::Expr*>& by) {
    if (!columns) return false;
    for (const auto* g : by)
      if (g->kind != sql::ExprKind::column) return false;
    return columns->grouped_by_index(grouped);
  };

  if (columns && has_mode(_variables.text("sql_mode"), "ONLY_FULL_GROUP_BY") && !per_row) {
    // What is grouped or fixed, and what follows from it (a key of a table
    // fixes its row).
    auto known = grouped;
    known.insert(known.end(), fixed.begin(), fixed.end());
    const auto determined = groups_by.empty() ? known : columns->determined(known);
    const auto dependent = [&](size_t i) { return has(determined, i); };
    const auto named = [&](size_t i) { return columns->named(i); };
    const std::string incompatible = "; this is incompatible with sql_mode=only_full_group_by";
    const auto check = [&](const sql::Expr* e, std::optional<size_t> column, size_t number, const char* clause) {
      for (const auto* g : groups_by)
        if (e && same_expr(*e, *g, columns)) return;
      std::vector<size_t> read;
      if (!e && column) read.push_back(*column);
      std::vector<const sql::Expr*> bare;
      if (e) bare_columns(*e, bare);
      for (const auto* b : bare)
        if (const auto i = columns->index(b->path)) read.push_back(*i);
      for (const auto i : read) {
        if (dependent(i)) continue;
        if (groups_by.empty())
          throw SqlError(1140, "42000",
                         "In aggregated query without GROUP BY, expression #" + std::to_string(number) + " of SELECT list contains nonaggregated column '" +
                             named(i) + "'" + incompatible);
        throw SqlError(1055, "42000",
                       "Expression #" + std::to_string(number) + " of " + clause + " is not in GROUP BY clause and contains nonaggregated column '" + named(i) +
                           "' which is not functionally dependent on columns in GROUP BY clause" + incompatible);
      }
    };
    for (size_t k = 0; k < outputs.size(); ++k) check(outputs[k].expr, outputs[k].column, k + 1, "SELECT list");
    if (!groups_by.empty())
      for (size_t k = 0; k < order_exprs.size(); ++k) {
        bool an_item = false;
        for (const auto& item : select.items) an_item = an_item || item.expr.get() == order_exprs[k];
        if (!an_item) check(order_exprs[k], std::nullopt, k + 1, "ORDER BY clause");
      }
  }

  // A condition that names no column (nor, in HAVING, an aggregate) is read
  // once, as MySQL folds it (observed: WHERE '1abc' over three rows warns
  // once).
  std::optional<bool> constant_where, constant_having;
  if (!_describing) {
    if (select.where && !mentions_column(*select.where)) constant_where = holds(rows, *select.where);
    if (select.having && !mentions_column(*select.having) && !contains_aggregate(*select.having)) constant_having = holds(having, *select.having);
  }
  const bool impossible = (constant_where && !*constant_where) || (constant_having && !*constant_having);

  // What the columns report. Grouping through a temporary table (the usual
  // way) reports what it holds as that table's fields; a group WHERE fixes,
  // a DISTINCT aggregate, or a condition found false needs none (observed:
  // HAVING FALSE reports COUNT(*) BINARY, as its own type).
  bool temporary = !groups_by.empty() && !impossible;
  if (temporary) {
    bool all_fixed = true;
    for (const auto* g : groups_by) all_fixed = all_fixed && g->kind == sql::ExprKind::column && has(fixed, *columns->index(g->path));
    bool distinct = false;
    for (const auto& spec : specs) distinct = distinct || spec.call().distinct;
    // SELECT DISTINCT goes through one all the same.
    temporary = !all_fixed && !distinct && (select.distinct || !grouped_by_index(groups_by));
  }
  ResultSet result;
  for (const auto& o : outputs) {
    if (o.column) {
      auto column = columns->column(*o.column, o.name);
      if (implicit) column.type.flags = static_cast<uint16_t>(column.type.flags & ~c::not_null);
      if (temporary && column.type.nullable() && has(grouped, *o.column)) column.type.flags = static_cast<uint16_t>(column.type.flags | group_flag);
      // A join's temporary table holds no keys (observed).
      if (temporary && join) column.type = without_keys(column.type);
      result.columns.push_back(std::move(column));
      continue;
    }
    auto type = o.type;
    if (temporary) {
      const auto& e = *o.expr;
      if (is_aggregate(e)) {
        const bool min_max = e.value == "MIN" || e.value == "MAX";
        const auto argument = min_max && e.args[0]->kind == sql::ExprKind::column ? columns->index(e.args[0]->path) : std::nullopt;
        if (argument) {
          type = columns->column(*argument, "").type;
          type.flags = static_cast<uint16_t>(type.flags & ~(c::not_null | c::primary_key | c::unique_key | c::multiple_key | c::part_key | c::auto_increment));
        } else if (e.value != "AVG") {
          type = as_field(type);
        }
      } else if (!contains_aggregate(e) && mentions_column(e)) {
        type = as_field(type);
      }
    }
    if (_view && through_temporary_table(select) && !contains_aggregate(*o.expr) && mentions_column(*o.expr)) type = as_view_field(type);
    result.columns.push_back({o.name, type});
  }

  if (_describing) return described(std::move(result));
  const uint64_t limit = select.limit ? limit_of(*select.limit) : _variables.number("sql_select_limit");
  const uint64_t offset = select.offset ? limit_of(*select.offset) : 0;
  std::vector<uint16_t> key_collations;
  for (const auto* g : groups_by) key_collations.push_back(key_collation(*g, rows.type(*g), columns, settings.result_collation));
  // A group's first row, by its number among the rows read; none for an
  // implicit group of no rows.
  constexpr size_t no_row = std::numeric_limits<size_t>::max();
  struct Group {
    size_t row = no_row;
    std::vector<Accumulator> sums;
  };
  std::vector<Group> groups;
  std::map<std::vector<Value>, size_t, ValuesLess> found(ValuesLess{&key_collations});
  const auto new_group = [&](size_t row) {
    Group g{row, {}};
    for (const auto& spec : specs) g.sums.emplace_back(spec);
    groups.push_back(std::move(g));
    return groups.size() - 1;
  };
  if (implicit) new_group(no_row);

  txn::Transaction* t = (table && !_view) || join ? &transaction() : nullptr;
  std::vector<txn::Row> table_rows;
  std::vector<Joined> joined_rows;
  const auto set_row = [&](size_t k) {
    if (join) join->set(k == no_row ? nullptr : &joined_rows[k]);
    if (table_columns) table_columns->set_row(k == no_row ? nullptr : &table_rows[k]);
  };
  const sql::Select::Locking* clause = nullptr;
  std::vector<const sql::Select::Locking*> join_clauses;
  bool join_locking = false;
  try {
    if (join) {
      std::vector<std::string> qualifiers;
      for (const auto& leaf : join->leaves) qualifiers.push_back(leaf->columns->qualifier());
      join_clauses = locking_clauses(select, qualifiers);
      join_locking = read_join(*join, join_clauses, *t);
      joined_rows = join->rows();
    } else if (t) {
      clause = locking_clause(select, table_columns->qualifier());
    }
    if (join) {
      // Its rows are read above.
    } else if (t && clause) {
      // A locking read: the current rows, those WHERE keeps locked.
      begin_reading(*t);
      if (!_insert_source) _rows.begin_statement(*t);
      table_rows = locked_rows(*clause, *t, *table, key_range(select.where.get(), *table, *table_columns, rows), [&](const txn::Row& r) {
        table_columns->set_row(&r);
        return !select.where || (constant_where ? *constant_where : holds(rows, *select.where));
      });
      table_columns->set_row(nullptr);
    } else if (t) {
      begin_reading(*t);
      table_rows = _rows.rows(*t, table->id);
    } else if (_view) {
      table_rows = _view->rows;
    } else {
      // A SELECT without FROM reads one row of nothing.
      table_rows.emplace_back();
    }
    const size_t row_count = join ? joined_rows.size() : table_rows.size();
    std::vector<const Joined*> passed;
    for (size_t k = 0; k < row_count; ++k) {
      set_row(k);
      if (select.where && !(constant_where ? *constant_where : holds(rows, *select.where))) continue;
      if (join_locking) passed.push_back(&joined_rows[k]);
      size_t g = 0;
      if (per_row) {
        g = new_group(k);
      } else if (!implicit) {
        std::vector<Value> keys;
        for (const auto* e : groups_by) keys.push_back(rows.value(*e));
        const auto at = found.find(keys);
        if (at == found.end()) {
          g = new_group(k);
          found.emplace(std::move(keys), g);
        } else {
          g = at->second;
        }
      } else if (groups[0].row == no_row) {
        groups[0].row = k;
      }
      for (size_t s = 0; s < specs.size(); ++s) {
        std::vector<Value> args;
        for (const auto& arg : specs[s].call().args) args.push_back(rows.value(*arg));
        groups[g].sums[s].add(args, rows);
      }
    }
    // A locking read over a join locks the rows WHERE kept.
    if (join_locking) lock_join(*join, join_clauses, *t, passed, select.where.get());

    struct Chosen {
      std::vector<Value> keys;
      std::vector<Value> values;
      std::vector<std::optional<std::string>> texts;
    };
    std::vector<Chosen> chosen;
    for (const auto& g : groups) {
      set_row(g.row);
      aggregates.set_group(&g.sums);
      if (select.having && !(constant_having ? *constant_having : holds(having, *select.having))) continue;
      Chosen ch;
      for (const auto& sort : sorts) ch.keys.push_back(items.value(*sort.expr));
      for (size_t k = 0; k < outputs.size(); ++k) {
        const auto& o = outputs[k];
        if (o.column) {
          ch.values.push_back(columns->value_at(*o.column));
          ch.texts.push_back(columns->text(*o.column));
        } else {
          ch.values.push_back(items.value(*o.expr));
          ch.texts.push_back(padded_text(ch.values.back(), result.columns[k].type));
        }
      }
      chosen.push_back(std::move(ch));
    }
    if (select.distinct) {
      std::vector<uint16_t> collations;
      for (const auto& o : outputs)
        collations.push_back(o.column ? columns->collation_at(*o.column) : key_collation(*o.expr, o.type, columns, settings.result_collation));
      std::set<std::vector<Value>, ValuesLess> seen(ValuesLess{&collations});
      std::vector<Chosen> kept;
      for (auto& ch : chosen)
        if (seen.insert(ch.values).second) kept.push_back(std::move(ch));
      chosen = std::move(kept);
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
    for (size_t i = 0; i < chosen.size(); ++i)
      if (i >= offset && i - offset < limit) result.rows.push_back(std::move(chosen[i].texts));
    set_row(no_row);
  } catch (...) {
    set_row(no_row);
    // A locking read that could not finish gives back what it locked (an
    // INSERT's source, with the INSERT).
    if (t && (clause || join_locking) && !_insert_source) _rows.undo_statement(*t);
    if (t) end_statement(true);
    throw;
  }
  if (t) end_statement(false);
  _warnings = rows.warnings();
  for (const auto* e : {&items, &having}) _warnings.insert(_warnings.end(), e->warnings().begin(), e->warnings().end());
  if (join) {
    const auto more = join->warnings();
    _warnings.insert(_warnings.end(), more.begin(), more.end());
  }
  Outcome out;
  out.result = std::move(result);
  return out;
}

Outcome Session::insert(const sql::Statement& statement) {
  const auto& in = statement.insert;
  const auto database = database_of(in.table);
  refuse_information_schema(database);
  const auto table = _catalog.table({database, in.table.back()});
  if (!table) {
    if (!_catalog.database(database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
    throw SqlError(1146, "42S02", "Table '" + database + "." + in.table.back() + "' doesn't exist");
  }

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

  // ON DUPLICATE KEY UPDATE's assignments, read over the row a key holds and
  // the row being inserted, checked before any row (observed: an unknown
  // column is 1054 though nothing is duplicated).
  const auto upsert_settings_base = eval_settings();
  TableColumns held(*table, database, "", upsert_settings_base);
  UpsertColumns upsert(held, *table, upsert_settings_base, in);
  auto upsert_settings = upsert_settings_base;
  upsert_settings.columns = &upsert;
  Evaluator upserting(upsert_settings);
  struct UpsertSet {
    size_t column;
    const sql::Expr* value;
  };
  std::vector<UpsertSet> upsert_sets;
  bool deprecated_values = false;
  for (const auto& [path, value] : in.on_duplicate) {
    const auto i = held.index(path);
    if (!i) {
      std::string name;
      for (size_t k = 0; k < path.size(); ++k) name += (k ? "." : "") + path[k];
      throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + name + "' in 'field list'");
    }
    if (value->kind != sql::ExprKind::default_value) upserting.type(*value);
    deprecated_values = deprecated_values || mentions_values(*value);
    upsert_sets.push_back({*i, value.get()});
  }
  if (deprecated_values)
    _warnings.push_back({"Warning", 1287,
                         "'VALUES function' is deprecated and will be removed in a future release. Please use an alias (INSERT INTO ... VALUES (...) AS "
                         "alias) and replace VALUES(col) in the ON DUPLICATE KEY UPDATE clause with alias.col instead"});

  auto& t = transaction();
  _rows.begin_statement(t);
  Outcome outcome;
  // Rows INSERT IGNORE skipped, REPLACE deleted, or ON DUPLICATE KEY UPDATE
  // met, of the rows given.
  uint64_t duplicates = 0;
  size_t records = in.rows.size();
  try {
    Evaluator evaluator(eval_settings());
    // INSERT ... SELECT: the source read whole first, inside this statement
    // and as FOR SHARE (so a table may feed itself, as in MySQL).
    std::optional<ResultSet> source;
    if (in.select) {
      auto before = std::move(_warnings);
      _insert_source = true;
      try {
        source = std::move(*select(*in.select).result);
      } catch (...) {
        _insert_source = false;
        throw;
      }
      _insert_source = false;
      before.insert(before.end(), _warnings.begin(), _warnings.end());
      _warnings = std::move(before);
      if (source->columns.size() != given.size()) throw SqlError(1136, "21S01", "Column count doesn't match value count at row 1");
    }
    const size_t row_count = source ? source->rows.size() : in.rows.size();
    records = row_count;
    const auto primary = table->primary_key();
    // AUTO_INCREMENT: the statement's first need reserves a value for each
    // of its rows, or for INSERT ... SELECT, whose rows are not counted ahead,
    // 1, then 2, then 4 and so on (observed: the values of a failed statement
    // are lost).
    uint64_t next_auto = 0, reserved = 0, first_auto = 0, explicit_auto = 0, batch = 1;
    std::optional<size_t> auto_column;
    for (size_t i = 0; i < table->columns.size(); ++i)
      if (table->columns[i].auto_increment) auto_column = i;
    // INSERT IGNORE: a row's errors that can be ignored are warnings, a NOT
    // NULL column given NULL or nothing takes its type's implicit default,
    // and a row a key already holds is skipped (observed on MySQL 8.4.13).
    const auto ignorable = [&](const SqlError& e, txn::Row& target, size_t i) {
      if (!in.ignore) throw e;
      _warnings.push_back({"Warning", e.code(), e.what()});
      target[i] = implicit_default(table->columns[i].type);
    };
    for (size_t r = 0; r < row_count; ++r) {
      txn::Row row(table->columns.size());
      std::vector<bool> set(table->columns.size(), false);
      std::optional<size_t> generate;
      const auto use_default = [&](size_t i) {
        const auto& column = table->columns[i];
        if (column.auto_increment) {
          generate = i;
        } else if (column.default_kind == catalog::Column::Default::value) {
          row[i] = column.default_value;
        } else if (column.default_kind == catalog::Column::Default::current_timestamp) {
          row[i] = clock_text(column);
        } else if (column.default_kind == catalog::Column::Default::none && !column.nullable) {
          ignorable(SqlError(1364, "HY000", "Field '" + column.name + "' doesn't have a default value"), row, i);
        }
        set[i] = true;
      };
      const size_t width = source ? source->columns.size() : in.rows[r].size();
      for (size_t j = 0; j < width; ++j) {
        const size_t i = given[j];
        const auto& column = table->columns[i];
        ResultType from;
        Value value;
        // A source column that is a table's own is copied field to field.
        bool field = false;
        if (source) {
          from = source->columns[j].type;
          value = value_from_text(source->rows[r][j], from);
          field = !source->columns[j].original_name.empty();
        } else {
          const auto& e = *in.rows[r][j];
          if (e.kind == sql::ExprKind::default_value) {
            use_default(i);
            continue;
          }
          from = evaluator.type(e);
          value = evaluator.value(e);
        }
        set[i] = true;
        if (is_null(value)) {
          if (column.auto_increment) {
            generate = i;
          } else if (!column.nullable) {
            ignorable(SqlError(1048, "23000", "Column '" + column.name + "' cannot be null"), row, i);
          }
          continue;
        }
        row[i] = stored_text(column, value, from, r + 1, _warnings, in.ignore, field, zone_seconds());
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
      uint64_t generated = 0;
      if (generate) {
        if (reserved == 0) {
          if (source) {
            reserved = batch;
            batch *= 2;
          } else {
            reserved = row_count - r;
          }
          next_auto = _rows.reserve_auto(table->id, table->auto_increment, reserved);
        }
        generated = next_auto++;
        --reserved;
        if (first_auto == 0) first_auto = generated;
        row[*generate] = std::to_string(generated);
      }
      std::string key;
      if (primary) {
        key = encode_key(*table, table->keys[*primary], row);
      } else {
        key.assign(8, '\0');
        uint64_t id = _rows.next_row_id(table->id);
        for (int b = 7; b >= 0; --b, id >>= 8) key[static_cast<size_t>(b)] = static_cast<char>(id & 0xff);
      }
      const auto names = unique_names(*table, primary, row);
      for (;;) {
        std::string duplicate;
        uint64_t holder = 0;
        const auto inserted = _rows.insert(t, table->id, key, names, row, &duplicate, &holder);
        if (inserted == txn::Store::Inserted::inserted) {
          ++outcome.affected_rows;
          // A value given to the AUTO_INCREMENT column is the OK's id when
          // none was generated (observed).
          if (!generate && auto_column && row[*auto_column] && (*row[*auto_column])[0] != '-')
            explicit_auto = std::strtoull(row[*auto_column]->c_str(), nullptr, 10);
          break;
        }
        if (inserted == txn::Store::Inserted::locked) throw LockWaitNeeded(t.id(), holder);
        if (!upsert_sets.empty()) {
          // ON DUPLICATE KEY UPDATE: the row holding the key, read as UPDATE
          // reads, is updated as the assignments say; 2 affected when it
          // changes, 0 when not, and its value given to the AUTO_INCREMENT
          // column is the OK's id (observed).
          std::optional<txn::Store::Keyed> found;
          for (auto& held_row : _rows.current(t, table->id)) {
            const auto theirs = unique_names(*table, primary, held_row.row);
            if (duplicate.empty() ? held_row.key == key : std::find(theirs.begin(), theirs.end(), duplicate) != theirs.end()) {
              found = std::move(held_row);
              break;
            }
          }
          ++duplicates;
          if (generate) {
            --next_auto;
            ++reserved;
            if (first_auto == generated) first_auto = 0;
          }
          if (!found) break;
          txn::Row updated = found->row;
          held.set_row(&updated);
          upsert.set_inserted(&row);
          try {
            // Each value sees the columns set before it.
            for (const auto& assignment : upsert_sets) {
              const size_t i = assignment.column;
              const auto& column = table->columns[i];
              if (assignment.value->kind == sql::ExprKind::default_value) {
                if (column.default_kind == catalog::Column::Default::value) {
                  updated[i] = column.default_value;
                } else if (column.default_kind == catalog::Column::Default::current_timestamp) {
                  updated[i] = clock_text(column);
                } else if (column.default_kind == catalog::Column::Default::none && !column.nullable) {
                  ignorable(SqlError(1364, "HY000", "Field '" + column.name + "' doesn't have a default value"), updated, i);
                } else {
                  updated[i].reset();
                }
                continue;
              }
              const auto from = upserting.type(*assignment.value);
              const auto value = upserting.value(*assignment.value);
              if (is_null(value)) {
                if (!column.nullable) {
                  ignorable(SqlError(1048, "23000", "Column '" + column.name + "' cannot be null"), updated, i);
                } else {
                  updated[i].reset();
                }
                continue;
              }
              updated[i] = stored_text(column, value, from, r + 1, _warnings, in.ignore, false, zone_seconds());
              if (column.auto_increment && (*updated[i])[0] != '-') _rows.saw_auto(table->id, table->auto_increment, std::stoull(*updated[i]));
            }
          } catch (...) {
            held.set_row(nullptr);
            upsert.set_inserted(nullptr);
            throw;
          }
          held.set_row(nullptr);
          upsert.set_inserted(nullptr);
          if (updated == found->row) break;
          {
            std::vector<bool> set_columns(table->columns.size(), false);
            for (const auto& assignment : upsert_sets) set_columns[assignment.column] = true;
            stamp(*table, found->row, updated, set_columns);
          }
          const std::string new_key = primary ? encode_key(*table, table->keys[*primary], updated) : found->key;
          std::string clash;
          switch (_rows.update(t, table->id, found->key, new_key, unique_names(*table, primary, updated), updated, &clash, &holder)) {
            case txn::Store::Inserted::inserted:
              outcome.affected_rows += 2;
              if (auto_column && updated[*auto_column] && (*updated[*auto_column])[0] != '-')
                explicit_auto = std::strtoull(updated[*auto_column]->c_str(), nullptr, 10);
              break;
            case txn::Store::Inserted::locked:
              throw LockWaitNeeded(t.id(), holder);
            case txn::Store::Inserted::duplicate: {
              const auto e = duplicate_error(*table, primary, clash, updated);
              if (!in.ignore) throw e;
              _warnings.push_back({"Warning", e.code(), e.what()});
            }
          }
          break;
        }
        if (in.replace) {
          // REPLACE: the row holding the key goes, read as UPDATE reads, and
          // the row is tried again; each counts (observed).
          std::string victim = key;
          if (!duplicate.empty()) {
            for (const auto& candidate : _rows.current(t, table->id)) {
              const auto theirs = unique_names(*table, primary, candidate.row);
              if (std::find(theirs.begin(), theirs.end(), duplicate) != theirs.end()) {
                victim = candidate.key;
                break;
              }
            }
          }
          if (!_rows.remove(t, table->id, victim, &holder)) throw LockWaitNeeded(t.id(), holder);
          ++outcome.affected_rows;
          ++duplicates;
          continue;
        }
        const auto e = duplicate_error(*table, primary, duplicate, row);
        if (!in.ignore) throw e;
        // Skipped; the value it was generated is the next row's.
        _warnings.push_back({"Warning", e.code(), e.what()});
        ++duplicates;
        if (generate) {
          --next_auto;
          ++reserved;
          if (first_auto == generated) first_auto = 0;
        }
        break;
      }
    }
    if (first_auto) {
      _last_insert_id = first_auto;
      outcome.last_insert_id = first_auto;
    } else if (explicit_auto && !source) {
      // INSERT ... SELECT reports only a generated value (observed).
      outcome.last_insert_id = explicit_auto;
    }
  } catch (...) {
    _rows.undo_statement(t);
    end_statement(true);
    throw;
  }
  end_statement(false);
  // Several rows, or any from a SELECT, are counted (observed).
  if (records > 1 || in.select)
    outcome.info = "Records: " + std::to_string(records) + "  Duplicates: " + std::to_string(duplicates) + "  Warnings: " + std::to_string(_warnings.size());
  return outcome;
}

void Session::assign(const catalog::Table& table, size_t index, const sql::Expr& value, txn::Row& row, Evaluator& evaluator, uint64_t number) {
  const auto& column = table.columns[index];
  if (value.kind == sql::ExprKind::default_value) {
    if (column.default_kind == catalog::Column::Default::current_timestamp) {
      row[index] = clock_text(column);
    } else if (column.default_kind == catalog::Column::Default::value) {
      row[index] = column.default_value;
    } else if (column.default_kind == catalog::Column::Default::none && !column.nullable) {
      throw SqlError(1364, "HY000", "Field '" + column.name + "' doesn't have a default value");
    } else {
      row[index].reset();
    }
    return;
  }
  const auto from = evaluator.type(value);
  const auto v = evaluator.value(value);
  if (is_null(v)) {
    if (!column.nullable) throw SqlError(1048, "23000", "Column '" + column.name + "' cannot be null");
    row[index].reset();
    return;
  }
  row[index] = stored_text(column, v, from, number, _warnings, false, false, zone_seconds());
  if (column.auto_increment && (*row[index])[0] != '-') _rows.saw_auto(table.id, table.auto_increment, std::stoull(*row[index]));
}

// Multiple-table UPDATE and DELETE (the manual's UPDATE and DELETE
// statements; observed on MySQL 8.4.13): the tables joined as a SELECT joins
// them; the tables changed (UPDATE's SET, DELETE's targets) read and locked
// as UPDATE reads, the others with shared locks; each row of a changed
// table that a joined row WHERE keeps holds is changed once, by the first
// such joined row. UPDATE takes no ORDER BY or LIMIT (1221).
Outcome Session::change_joined(const sql::Statement& statement) {
  const bool deleting = statement.kind == sql::Statement::Kind::delete_;
  const auto& c = statement.change;
  auto settings = eval_settings();
  auto from = join_from(
      c.from, _catalog, [&](const std::vector<std::string>& name) { return database_of(name); }, settings,
      [](const sql::TableRef&) -> View {
        throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'derived tables in multiple-table UPDATE and DELETE'");
      });
  auto& leaves = from->leaves;
  for (const auto& leaf : leaves) refuse_information_schema(leaf->database);
  const JoinedColumns& columns = *from->columns;
  settings.columns = &columns;
  Evaluator evaluator(settings);
  if (!deleting && !c.order.empty()) throw SqlError(1221, "HY000", "Incorrect usage of UPDATE and ORDER BY");
  if (!deleting && c.limit) throw SqlError(1221, "HY000", "Incorrect usage of UPDATE and LIMIT");

  // The tables changed, in the order first named; UPDATE's SET by table.
  std::vector<size_t> changed;
  struct Set {
    JoinPlace column;
    const sql::Expr* value;
  };
  std::vector<Set> sets;
  if (deleting) {
    for (const auto& target : c.targets) {
      std::optional<size_t> found;
      for (size_t l = 0; l < leaves.size() && !found; ++l) {
        const auto& leaf = *leaves[l];
        const bool bare = leaf.columns->qualifier() == leaf.table.name;
        if ((target.size() == 1 && target[0] == leaf.columns->qualifier()) ||
            (target.size() == 2 && bare && target[0] == leaf.database && target[1] == leaf.table.name))
          found = l;
      }
      if (!found) throw SqlError(1109, "42S02", "Unknown table '" + target.back() + "' in MULTI DELETE");
      if (std::find(changed.begin(), changed.end(), *found) == changed.end()) changed.push_back(*found);
    }
  } else {
    for (const auto& [path, value] : c.set) {
      const auto p = columns.place(path);
      if (!p) {
        std::string name;
        for (size_t k = 0; k < path.size(); ++k) name += (k ? "." : "") + path[k];
        throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + name + "' in 'field list'");
      }
      if (value->kind != sql::ExprKind::default_value) evaluator.type(*value);
      sets.push_back({*p, value.get()});
      if (std::find(changed.begin(), changed.end(), p->leaf) == changed.end()) changed.push_back(p->leaf);
    }
  }
  if (c.where) in_clause("where clause", [&] { evaluator.type(*c.where); });

  // Changed tables locked as UPDATE locks them, the others shared.
  static const sql::Select::Locking exclusive{true, {}, sql::Select::Locking::Wait::wait};
  static const sql::Select::Locking shared;
  std::vector<const sql::Select::Locking*> clauses(leaves.size(), &shared);
  for (const auto l : changed) clauses[l] = &exclusive;

  auto& t = transaction();
  Outcome outcome;
  uint64_t matched = 0;
  try {
    read_join(*from, clauses, t);
    const auto modes = _variables.text("sql_mode");
    const bool strict = has_mode(modes, "STRICT_TRANS_TABLES") || has_mode(modes, "STRICT_ALL_TABLES");
    const auto passes = [&] {
      const size_t held = evaluator.warnings().size();
      const bool b = holds(evaluator, *c.where);
      if (strict)
        for (size_t k = held; k < evaluator.warnings().size(); ++k)
          if (evaluator.warnings()[k].code == er_truncated_wrong_value) throw SqlError(er_truncated_wrong_value, "22007", evaluator.warnings()[k].message);
      return b;
    };
    std::optional<bool> constant_where;
    if (c.where && !mentions_column(*c.where)) constant_where = passes();
    auto joined = from->rows();
    std::vector<const Joined*> kept;
    for (const auto& j : joined) {
      from->set(&j);
      if (c.where && !(constant_where ? *constant_where : passes())) continue;
      kept.push_back(&j);
    }
    lock_join(*from, clauses, t, kept, c.where.get());

    // Each changed table's rows, each once, by the first joined row that
    // holds it.
    std::vector<std::set<const txn::Row*>> done(leaves.size());
    for (const auto* j : kept) {
      for (const auto l : changed) {
        auto& leaf = *leaves[l];
        const auto* old_row = (*j)[l];
        if (!old_row || !done[l].insert(old_row).second) continue;
        ++matched;
        const auto& key = leaf.keys[static_cast<size_t>(old_row - leaf.rows.data())];
        uint64_t holder = 0;
        if (deleting) {
          if (!_rows.remove(t, leaf.table.id, key, &holder)) throw LockWaitNeeded(t.id(), holder);
          ++outcome.affected_rows;
          continue;
        }
        // The joined row as read, but this table's row as SET has it so
        // far: each value sees the columns set before it.
        from->set(j);
        txn::Row row = *old_row;
        leaf.columns->set_row(&row);
        std::vector<bool> set_columns(leaf.table.columns.size(), false);
        for (const auto& set : sets)
          if (set.column.leaf == l) {
            assign(leaf.table, set.column.column, *set.value, row, evaluator, matched);
            set_columns[set.column.column] = true;
          }
        stamp(leaf.table, *old_row, row, set_columns);
        leaf.columns->set_row(old_row);
        if (row != *old_row) ++outcome.affected_rows;
        const auto primary = leaf.table.primary_key();
        const std::string new_key = primary ? encode_key(leaf.table, leaf.table.keys[*primary], row) : key;
        std::string duplicate;
        switch (_rows.update(t, leaf.table.id, key, new_key, unique_names(leaf.table, primary, row), row, &duplicate, &holder)) {
          case txn::Store::Inserted::inserted:
            break;
          case txn::Store::Inserted::locked:
            throw LockWaitNeeded(t.id(), holder);
          case txn::Store::Inserted::duplicate:
            duplicate_entry(leaf.table, primary, duplicate, row);
        }
      }
    }
    from->set(nullptr);
  } catch (...) {
    from->set(nullptr);
    _rows.undo_statement(t);
    end_statement(true);
    throw;
  }
  end_statement(false);
  _warnings.insert(_warnings.end(), evaluator.warnings().begin(), evaluator.warnings().end());
  const auto more = from->warnings();
  _warnings.insert(_warnings.end(), more.begin(), more.end());
  if (!deleting)
    outcome.info =
        "Rows matched: " + std::to_string(matched) + "  Changed: " + std::to_string(outcome.affected_rows) + "  Warnings: " + std::to_string(_warnings.size());
  return outcome;
}

Outcome Session::change_rows(const sql::Statement& statement) {
  const bool deleting = statement.kind == sql::Statement::Kind::delete_;
  const auto& c = statement.change;
  if (c.multiple()) return change_joined(statement);
  const auto database = database_of(c.table.name);
  refuse_information_schema(database);
  const auto table = _catalog.table({database, c.table.name.back()});
  if (!table) {
    if (!_catalog.database(database)) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + database + "'");
    throw SqlError(1146, "42S02", "Table '" + database + "." + c.table.name.back() + "' doesn't exist");
  }
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
    // Strict mode makes a string WHERE reads as a number with text left over
    // an error (observed: 1292, SQLSTATE 22007); a WHERE that names no
    // column is read once.
    const auto modes = _variables.text("sql_mode");
    const bool strict = has_mode(modes, "STRICT_TRANS_TABLES") || has_mode(modes, "STRICT_ALL_TABLES");
    const auto passes = [&] {
      const size_t held = evaluator.warnings().size();
      const bool b = holds(evaluator, *c.where);
      if (strict)
        for (size_t k = held; k < evaluator.warnings().size(); ++k)
          if (evaluator.warnings()[k].code == er_truncated_wrong_value) throw SqlError(er_truncated_wrong_value, "22007", evaluator.warnings()[k].message);
      return b;
    };
    std::optional<bool> constant_where;
    if (c.where && !mentions_column(*c.where)) constant_where = passes();
    for (size_t i = 0; i < rows.size(); ++i) {
      columns.set_row(&rows[i].row);
      if (c.where && !(constant_where ? *constant_where : passes())) continue;
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
      for (const auto& set : sets) assign(*table, set.column, *set.value, row, evaluator, matched);
      std::vector<bool> set_columns(table->columns.size(), false);
      for (const auto& set : sets) set_columns[set.column] = true;
      stamp(*table, old_row, row, set_columns);
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

namespace {

// A set operation's result as the columns its ORDER BY reads: each by the
// first part's name, unqualified.
class SetColumns : public ColumnSource {
 public:
  SetColumns(const ResultSet& result, const std::vector<uint16_t>& collations) : _result(result), _collations(collations) {}
  std::optional<Found> find_column(const std::vector<std::string>& path) const override {
    const auto i = index(path);
    if (!i) return std::nullopt;
    return Found{_result.columns[*i].type, _collations[*i]};
  }
  Value column_value(const std::vector<std::string>& path) const override { return value_from_text((*_row)[*index(path)], _result.columns[*index(path)].type); }
  void set_row(const std::vector<std::optional<std::string>>* row) { _row = row; }

 private:
  std::optional<size_t> index(const std::vector<std::string>& path) const {
    if (path.size() != 1) return std::nullopt;
    for (size_t i = 0; i < _result.columns.size(); ++i)
      if (sql::same_word(_result.columns[i].name, path[0])) return i;
    return std::nullopt;
  }
  const ResultSet& _result;
  const std::vector<uint16_t>& _collations;
  const std::vector<std::optional<std::string>>* _row = nullptr;
};

}  // namespace

Session::SetPart Session::set_part(const sql::Select& query, std::vector<Warning>& warnings) {
  SetPart part;
  if (query.set) {
    part.result = *select_set(query).result;
    warnings.insert(warnings.end(), _warnings.begin(), _warnings.end());
    // A set operation's column holds its joined collation, as a column does.
    for (const auto& column : part.result.columns) part.derivations.emplace_back(column.type.collation, implicit_coercibility);
    return part;
  }
  part.result = *select(query).result;
  warnings.insert(warnings.end(), _warnings.begin(), _warnings.end());
  for (size_t i = 0; i < part.result.columns.size(); ++i) {
    const sql::Expr* e = i < query.items.size() && !query.items[i].star ? query.items[i].expr.get() : nullptr;
    // COLLATE is explicit, a string literal coercible, anything else as a
    // column holds its collation.
    const int coercibility = e && e->kind == sql::ExprKind::collate  ? explicit_coercibility
                             : e && e->kind == sql::ExprKind::string ? coercible_coercibility
                                                                     : implicit_coercibility;
    part.derivations.emplace_back(part.result.columns[i].type.collation, coercibility);
  }
  return part;
}

Outcome Session::select_set(const sql::Select& query) {
  using Op = sql::Select::SetOperation::Op;
  const auto& set = *query.set;
  std::vector<Warning> warnings;
  std::vector<SetPart> parts;
  parts.push_back(set_part(set.left, warnings));
  if (set.op != Op::none) {
    parts.push_back(set_part(set.right, warnings));
    if (parts[0].result.columns.size() != parts[1].result.columns.size())
      throw SqlError(1222, "21000", "The used SELECT statements have a different number of columns");
  }
  const auto settings = eval_settings();
  Evaluator evaluator(settings);
  // Each column: the first part's name, the parts' types joined, strings
  // under their collations aggregated.
  ResultSet result;
  std::vector<std::pair<uint16_t, int>> derivations;
  std::vector<uint16_t> collations;
  for (size_t i = 0; i < parts[0].result.columns.size(); ++i) {
    std::vector<ResultType> types;
    std::vector<std::pair<uint16_t, int>> strings;
    for (const auto& part : parts) {
      const auto& type = part.result.columns[i].type;
      types.push_back(type);
      if (is_string_type(type) && type.field_type != f::null) strings.push_back(part.derivations[i]);
    }
    const auto derivation = strings.empty() ? std::pair<uint16_t, int>{settings.result_collation, implicit_coercibility}
                                            : evaluator.aggregated_collation(strings, set.op == Op::intersect ? "INTERSECT"
                                                                                      : set.op == Op::except  ? "EXCEPT"
                                                                                                              : "UNION");
    auto column = parts[0].result.columns[i];
    // A parenthesised query alone keeps its columns (no temporary table).
    if (set.op != Op::none) column.type = joined_type(types, derivation.first);
    column.schema.clear();
    column.table.clear();
    column.original_table.clear();
    column.original_name.clear();
    result.columns.push_back(std::move(column));
    derivations.push_back(derivation);
    collations.push_back(is_string_type(result.columns.back().type) ? result.columns.back().type.collation : 0);
  }

  // The parts' rows in the joined types, compared under its collations.
  const auto today = [&] {
    auto m = temporal::from_unix_us(settings.statement_us + static_cast<int64_t>(settings.zone_seconds) * 1000000);
    m.hour = m.minute = m.second = m.micro = 0;
    return m;
  };
  const auto as_joined = [&](const std::optional<std::string>& text, const ResultType& from, const ResultType& to) -> std::optional<std::string> {
    if (!text || (from.field_type == to.field_type && from.decimals == to.decimals)) return text;
    switch (to.field_type) {
      case f::tiny:
      case f::short_:
      case f::int24:
      case f::long_:
      case f::longlong: {
        // Within the field's range, as the temporary table stores it
        // (observed: YEAR 2024 in a TINYINT is 127).
        const auto ft = from.field_type;
        const bool integral = ft == f::tiny || ft == f::short_ || ft == f::int24 || ft == f::long_ || ft == f::longlong || ft == f::year;
        if (to.field_type == f::longlong || !integral) return text;
        const int bits = to.field_type == f::tiny ? 8 : to.field_type == f::short_ ? 16 : to.field_type == f::int24 ? 24 : 32;
        const int64_t most = to.is_unsigned() ? (int64_t{1} << bits) - 1 : (int64_t{1} << (bits - 1)) - 1;
        const int64_t least = to.is_unsigned() ? 0 : -(int64_t{1} << (bits - 1));
        const auto v = std::strtoll(text->c_str(), nullptr, 10);
        return std::to_string(std::clamp<int64_t>(v, least, most));
      }
      case f::newdecimal:
        if (const auto d = Decimal::parse(*text)) return d->rounded(to.decimals).to_string();
        return text;
      case f::float_:
      case f::double_:
        return to_text(Value{std::strtod(text->c_str(), nullptr)}, to);
      case f::date:
      case f::time:
      case f::datetime:
      case f::timestamp: {
        if (!is_temporal_type(from)) return text;
        const auto from_kind = temporal_kind_of(from), to_kind = temporal_kind_of(to);
        auto m = temporal::parse_text(*text, from_kind);
        if (from_kind == temporal::Kind::time && to_kind != temporal::Kind::time) {
          const int64_t into = ((static_cast<int64_t>(m.hour) * 60 + m.minute) * 60 + m.second) * 1000000 + m.micro;
          m = temporal::from_unix_us(temporal::to_unix_us(today()) + (m.negative ? -into : into));
        }
        return temporal::text(m, to_kind, to.decimals);
      }
      default:
        return text;
    }
  };
  using Row = std::vector<std::optional<std::string>>;
  const auto rows_of = [&](const SetPart& part) {
    std::vector<Row> out;
    for (const auto& row : part.result.rows) {
      Row converted;
      for (size_t i = 0; i < row.size(); ++i) converted.push_back(as_joined(row[i], part.result.columns[i].type, result.columns[i].type));
      out.push_back(std::move(converted));
    }
    return out;
  };
  const auto values_of = [&](const Row& row) {
    std::vector<Value> out;
    for (size_t i = 0; i < row.size(); ++i) out.push_back(value_from_text(row[i], result.columns[i].type));
    return out;
  };
  const ValuesLess less{&collations};
  // Each row once, the first met kept.
  const auto distinct = [&](std::vector<Row> rows) {
    std::set<std::vector<Value>, ValuesLess> seen(less);
    std::vector<Row> kept;
    for (auto& row : rows)
      if (seen.insert(values_of(row)).second) kept.push_back(std::move(row));
    return kept;
  };

  if (!_describing) {
    auto left = rows_of(parts[0]);
    if (set.op == Op::none) {
      result.rows = std::move(left);
    } else {
      auto right = rows_of(parts[1]);
      if (set.op == Op::union_) {
        left.insert(left.end(), std::make_move_iterator(right.begin()), std::make_move_iterator(right.end()));
        result.rows = set.all ? std::move(left) : distinct(std::move(left));
      } else {
        // INTERSECT keeps a left row the right side has, EXCEPT one it has
        // not; ALL counts each copy.
        std::map<std::vector<Value>, size_t, ValuesLess> counts(less);
        for (const auto& row : right) ++counts[values_of(row)];
        std::vector<Row> kept;
        for (auto& row : left) {
          auto it = counts.find(values_of(row));
          const bool found = it != counts.end() && it->second > 0;
          if (found && set.all) --it->second;
          if (found == (set.op == Op::intersect)) kept.push_back(std::move(row));
        }
        result.rows = set.all ? std::move(kept) : distinct(std::move(kept));
      }
    }
  }

  // The whole result's ORDER BY, over its columns by name or position, and
  // its LIMIT.
  SetColumns columns(result, collations);
  auto sort_settings = settings;
  sort_settings.columns = &columns;
  Evaluator sorter(sort_settings);
  struct Sort {
    const sql::Expr* expr = nullptr;
    std::optional<size_t> position;
    bool descending = false;
    uint16_t collation = 0;
  };
  std::vector<Sort> sorts;
  for (const auto& order : query.order) {
    Sort sort;
    sort.descending = order.descending;
    if (is_position(*order.expr)) {
      const auto n = std::stoull(order.expr->value);
      if (n < 1 || n > result.columns.size()) throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + order.expr->value + "' in 'order clause'");
      sort.position = n - 1;
      sort.collation = collations[n - 1];
    } else {
      sort.expr = order.expr.get();
      ResultType type;
      in_clause("order clause", [&] { type = sorter.type(*sort.expr); });
      sort.collation = order_collation(*sort.expr, type, columns, settings.result_collation);
    }
    sorts.push_back(sort);
  }
  _warnings = std::move(warnings);
  if (_describing) return described(std::move(result));
  const uint64_t limit = query.limit ? limit_of(*query.limit) : _variables.number("sql_select_limit");
  const uint64_t offset = query.offset ? limit_of(*query.offset) : 0;
  if (!sorts.empty()) {
    struct Keyed {
      Row row;
      std::vector<Value> keys;
    };
    std::vector<Keyed> keyed;
    for (auto& row : result.rows) {
      Keyed k{std::move(row), {}};
      columns.set_row(&k.row);
      for (const auto& sort : sorts)
        k.keys.push_back(sort.position ? value_from_text(k.row[*sort.position], result.columns[*sort.position].type) : sorter.value(*sort.expr));
      keyed.push_back(std::move(k));
    }
    columns.set_row(nullptr);
    std::stable_sort(keyed.begin(), keyed.end(), [&](const Keyed& a, const Keyed& b) {
      for (size_t i = 0; i < sorts.size(); ++i) {
        const int order = compare_for_order(a.keys[i], b.keys[i], sorts[i].collation);
        if (order != 0) return sorts[i].descending ? order > 0 : order < 0;
      }
      return false;
    });
    result.rows.clear();
    for (auto& k : keyed) result.rows.push_back(std::move(k.row));
    _warnings.insert(_warnings.end(), sorter.warnings().begin(), sorter.warnings().end());
  }
  std::vector<Row> limited;
  for (size_t i = 0; i < result.rows.size(); ++i)
    if (i >= offset && i - offset < limit) limited.push_back(std::move(result.rows[i]));
  result.rows = std::move(limited);
  Outcome out;
  out.result = std::move(result);
  return out;
}

Outcome Session::select(const sql::Select& select) {
  if (select.set) return select_set(select);
  if (!select.from.empty()) return select_from_table(select);
  if (is_grouped(select)) return select_groups(select, nullptr, "");
  Evaluator evaluator(eval_settings());
  ResultSet result;
  for (const auto& item : select.items) {
    if (item.star) throw SqlError(er_no_tables_used, "HY000", "No tables used");
    result.columns.push_back({column_name(item), evaluator.type(*item.expr)});
  }
  for (const auto& order : select.order) {
    // An item's alias names that item (observed: SELECT 3 a ORDER BY a).
    const auto& x = *order.expr;
    const bool alias = x.kind == sql::ExprKind::column && x.path.size() == 1 &&
                       std::any_of(select.items.begin(), select.items.end(),
                                   [&](const sql::SelectItem& item) { return item.has_alias && sql::same_word(item.alias, x.path[0]); });
    if (alias) continue;
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
  if (_describing) return described(std::move(result));
  const uint64_t limit = select.limit ? limit_of(*select.limit) : _variables.number("sql_select_limit");
  const uint64_t offset = select.offset ? limit_of(*select.offset) : 0;
  const bool passes = !select.where || holds(evaluator, *select.where);
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
      // One of a character set MySQL has, though Kvasir does not convert to
      // it, is still another set's (observed: 1253, not 1273).
      const bool foreign = !collation && charset_not_converted(std::string_view(collation_name).substr(0, collation_name.find('_')));
      if (!collation && !foreign) throw SqlError(1273, "HY000", "Unknown collation: '" + collation_name + "'");
      if (foreign || !sql::same_word(collation->charset, charset))
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
        // The results' collation is then its character set's default.
        if (sql::same_word(a.name, "character_set_results")) _results_collation.clear();
        if (sql::same_word(a.name, "character_set_connection") && !p.use_default) {
          if (const auto* d = default_collation(_variables.text("character_set_connection")))
            _variables.set("", "collation_connection", d->name, false, _warnings);
        }
        if (sql::same_word(a.name, "collation_connection")) {
          if (const auto* col = find_collation(_variables.text("collation_connection")))
            _variables.set("", "character_set_connection", col->charset, false, _warnings);
        }
        if (sql::same_word(a.name, "character_set_database") || sql::same_word(a.name, "collation_database")) {
          // DEFAULT is the current database's pair; a character set brings
          // its default collation, a collation its character set.
          if (p.use_default) {
            follow_database();
          } else if (sql::same_word(a.name, "character_set_database")) {
            if (const auto* d = default_collation(_variables.text("character_set_database"))) _variables.assign("collation_database", d->name);
          } else if (const auto* col = find_collation(_variables.text("collation_database"))) {
            _variables.assign("character_set_database", col->charset);
          }
          _warnings.push_back({"Warning", 1681, "Updating '" + lower(a.name) + "' is deprecated. It will be made read-only in a future release."});
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
        _results_collation = collation->name;
        break;
      }
      case Target::character_set: {
        const std::string charset = a.use_default ? _variables.text("character_set_server") : a.charset;
        _variables.set("", "character_set_client", charset, false, _warnings);
        _variables.set("", "character_set_results", charset, false, _warnings);
        _results_collation.clear();
        _variables.set("", "character_set_connection", _variables.text("character_set_server"), false, _warnings);
        _variables.set("", "collation_connection", _variables.text("collation_server"), false, _warnings);
        break;
      }
    }
  }
}

void Session::refuse_information_schema(const std::string& database) const {
  if (is_information_schema(database))
    throw SqlError(1044, "42000",
                   "Access denied for user '" + _info.user + "'@'" + _info.host + "' to database '" + std::string(information_schema_name) + "'");
}

void Session::follow_database() {
  std::string charset = _variables.text("character_set_server");
  std::string collation = _variables.text("collation_server");
  if (_database && is_information_schema(*_database)) {
    // As SCHEMATA shows it.
    charset = "utf8mb3";
    collation = "utf8mb3_general_ci";
  } else if (_database) {
    if (const auto database = _catalog.database(*_database)) {
      charset = database->charset;
      collation = database->collation;
    }
  }
  _variables.assign("character_set_database", std::move(charset));
  _variables.assign("collation_database", std::move(collation));
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
    if (charset_not_converted(charset)) {
      // A collation of another character set is refused first (observed:
      // utf8mb3 with utf8mb4_bin is 1253), naming the set by its own name.
      const std::string named = sql::same_word(charset, "utf8") ? std::string("utf8mb3") : lower(charset);
      if (const auto* info = collation.empty() ? nullptr : find_collation(collation); info && info->charset != named)
        throw SqlError(1253, "42000", "COLLATION '" + collation + "' is not valid for CHARACTER SET '" + named + "'");
      throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'character set " + lower(charset) + "'");
    }
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
    follow_database();
  }
  outcome.affected_rows = tables;
  return outcome;
}

Outcome Session::show_databases(const sql::Statement& statement) {
  // A name, held in utf8mb3_bin (observed).
  ResultType type;
  type.field_type = f::var_string;
  type.collation = name_collation;
  type.length = 192;
  type.decimals = 0;
  type.flags = c::not_null | c::binary | c::no_default_value;
  ResultSet result;
  result.columns.push_back({statement.like ? "Database (" + *statement.like + ")" : "Database", type});
  // Names are matched as bytes, as they are compared.
  const auto* bytes = collation_by_id(46);
  // information_schema among them, in byte order.
  auto names = _catalog.database_names();
  names.insert(std::lower_bound(names.begin(), names.end(), std::string(information_schema_name)), std::string(information_schema_name));
  for (const auto& name : names)
    if (!statement.like || bytes->like(name, *statement.like, "\\")) result.rows.push_back({name});
  Outcome outcome;
  outcome.result = std::move(result);
  return outcome;
}

Outcome Session::create_table(const sql::Statement& statement) {
  const auto& spec = statement.create_table;
  const auto& name = spec.name.back();
  const auto database = database_of(spec.name);
  refuse_information_schema(database);
  check_table_name(name);
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
    refuse_information_schema(name.database);
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
  type.collation = name_collation;
  type.length = 192;
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
