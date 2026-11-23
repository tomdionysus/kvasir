//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// DATE_FORMAT, TIME_FORMAT and the week functions, typed and evaluated as
// MySQL 8.4.13 does (the manual's "Date and Time Functions", as observed:
// TODO/2026-11-23-temporal-notes.md).

#include <cstdint>
#include <string>

#include "kvasir/exec/expression.h"
#include "kvasir/exec/temporal.h"
#include "kvasir/protocol/types.h"

namespace kvasir::exec {

namespace {

namespace c = protocol::column_flag;
using sql::Expr;

constexpr uint8_t any_decimals = protocol::not_fixed_decimals;

struct Arity {
  std::string_view name;
  size_t least, most;
};

constexpr Arity format_functions[] = {
    {"DATE_FORMAT", 2, 2}, {"TIME_FORMAT", 2, 2}, {"WEEK", 1, 2}, {"YEARWEEK", 1, 2}, {"WEEKOFYEAR", 1, 1},
};

const Arity* format_named(const std::string& name) {
  for (const auto& f : format_functions)
    if (f.name == name) return &f;
  return nullptr;
}

}  // namespace

std::optional<ResultType> Evaluator::format_function(const Expr& e) const {
  if (e.kind != sql::ExprKind::call) return std::nullopt;
  const auto* fn = format_named(e.value);
  if (!fn) return std::nullopt;
  if (e.args.size() < fn->least || e.args.size() > fn->most)
    throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + e.value + "'");
  for (const auto& arg : e.args) type(*arg);
  if (e.value == "YEARWEEK") return integer_type(6, false, false);
  if (e.value != "DATE_FORMAT" && e.value != "TIME_FORMAT") return integer_type(2, false, false);
  // A string: as long as its format can make it (observed: '%Y-%m-%d %H'
  // is 18 characters, %H counting 7), or ten times a format's own length
  // when the format is not a constant string.
  const auto& f = *e.args[1];
  const auto ft = type(f);
  ResultType t;
  t.field_type = protocol::field_type::var_string;
  t.collation = _settings.result_collation;
  t.decimals = any_decimals;
  t.flags = _settings.result_collation_binary ? c::binary : 0;
  uint32_t characters = 0;
  if (f.kind == sql::ExprKind::string)
    characters = temporal::format_length(f.value);
  else if (ft.field_type != protocol::field_type::null)
    characters = ft.length / (is_string_type(ft) ? widest_character(ft.collation) : 1) * 10;
  t.length = characters * _settings.result_max_bytes;
  return t;
}

Value Evaluator::format_function_value(const Expr& e) {
  format_function(e);
  if (e.value == "DATE_FORMAT" || e.value == "TIME_FORMAT") {
    const bool time_only = e.value == "TIME_FORMAT";
    const auto m = time_only ? time_argument(*e.args[0]) : datetime_argument(*e.args[0]);
    if (!m) return {};
    const auto ft = type(*e.args[1]);
    const auto fv = value(*e.args[1]);
    if (is_null(fv)) return {};
    const auto f = to_text(fv, ft).value_or("");
    // An empty format gives NULL (observed).
    if (f.empty()) return {};
    const auto out = temporal::format(*m, f, time_only);
    if (!out) return {};
    return *out;
  }
  const auto m = datetime_argument(*e.args[0]);
  if (!m) return {};
  // WEEK's mode, NULL as 0 (observed); WEEKOFYEAR is mode 3.
  int64_t mode = e.value == "WEEKOFYEAR" ? 3 : 0;
  if (e.args.size() == 2) mode = integer_argument(*e.args[1]).value_or(0);
  uint32_t behaviour = temporal::week_mode(mode);
  if (e.value == "YEARWEEK") behaviour |= temporal::week_year;
  uint32_t year = 0;
  const uint32_t week = temporal::week(*m, behaviour, &year);
  if (e.value == "YEARWEEK") return static_cast<int64_t>(year) * 100 + week;
  return static_cast<int64_t>(week);
}

}  // namespace kvasir::exec
