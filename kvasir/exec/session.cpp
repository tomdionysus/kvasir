//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/session.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <utility>

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

Session::Session(const VariableRegistry& registry, SessionInfo info) : _info(std::move(info)), _variables(registry) {}

EvalSettings Session::eval_settings() const {
  EvalSettings e;
  e.variables = this;
  e.div_precision_increment = static_cast<int>(_variables.number("div_precision_increment"));
  const auto* collation = find_collation(_variables.text("collation_connection"));
  e.result_collation = collation ? collation->id : protocol::collation::utf8mb4_0900_ai_ci;
  e.result_max_bytes = collation && collation->charset == "binary" ? 1 : 4;
  return e;
}

ResultType Session::system_type(const std::string& scope, const std::string& name) const {
  const auto e = eval_settings();
  return _variables.type_of(scope, name, e.result_collation, e.result_max_bytes);
}

Value Session::system_value(const std::string& scope, const std::string& name) const { return _variables.get(scope, name); }

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

void Session::commit() { _in_transaction = false; }

Outcome Session::execute(const sql::Statement& statement) {
  _warnings.clear();
  using Kind = sql::Statement::Kind;
  switch (statement.kind) {
    case Kind::select:
      return select(statement.select);
    case Kind::set:
      set(statement.assignments);
      return {};
    case Kind::use:
      throw SqlError(er_bad_db_error, "42000", "Unknown database '" + statement.database + "'");
    case Kind::begin:
      if (_in_transaction) commit();
      _in_transaction = true;
      _read_only_transaction = statement.read_only;
      return {};
    case Kind::commit:
      commit();
      return {};
    case Kind::rollback:
      _in_transaction = false;
      return {};
  }
  return {};
}

Outcome Session::select(const sql::Select& select) {
  if (!select.from.empty()) {
    const auto& table = select.from.front();
    if (table.name.size() == 2) throw SqlError(er_bad_db_error, "42000", "Unknown database '" + table.name[0] + "'");
    if (!_database) throw SqlError(er_no_db_error, "3D000", "No database selected");
    throw SqlError(1146, "42S02", "Table '" + *_database + "." + table.name[0] + "' doesn't exist");
  }
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
          if (const auto* d = default_collation(_variables.text("character_set_connection"))) _variables.set("", "collation_connection", d->name, false, _warnings);
        }
        if (sql::same_word(a.name, "collation_connection")) {
          if (const auto* col = find_collation(_variables.text("collation_connection"))) _variables.set("", "character_set_connection", col->charset, false, _warnings);
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

}  // namespace kvasir::exec
