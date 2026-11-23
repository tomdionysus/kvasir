//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The date and time functions over a value, typed and evaluated as MySQL
// 8.4.13 does (the manual's "Date and Time Functions", as observed:
// TODO/2026-11-23-temporal-notes.md).

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "kvasir/exec/expression.h"
#include "kvasir/exec/temporal.h"
#include "kvasir/protocol/types.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;
using sql::Expr;
using sql::ExprKind;
using temporal::Kind;
using temporal::Moment;

constexpr uint8_t any_decimals = protocol::not_fixed_decimals;
constexpr int64_t day_us = 86400LL * 1000000;
// Days from year 0 to 1970-01-01, as TO_DAYS counts them.
constexpr int64_t days_to_epoch = 719528;

// What a function reads its argument as, and what it gives.
enum class Reads : uint8_t { date, time };
enum class Gives : uint8_t { year, number, name, date, time };

struct DateFunction {
  std::string_view name;
  Reads reads;
  Gives gives;
  // An integer result's digits (its length one more).
  int digits;
  size_t arguments;
};

constexpr DateFunction date_functions[] = {
    {"YEAR", Reads::date, Gives::year, 4, 1},         {"MONTH", Reads::date, Gives::number, 2, 1},     {"DAY", Reads::date, Gives::number, 2, 1},
    {"DAYOFMONTH", Reads::date, Gives::number, 2, 1}, {"DAYOFWEEK", Reads::date, Gives::number, 1, 1}, {"WEEKDAY", Reads::date, Gives::number, 1, 1},
    {"DAYOFYEAR", Reads::date, Gives::number, 3, 1},  {"QUARTER", Reads::date, Gives::number, 1, 1},   {"TO_DAYS", Reads::date, Gives::number, 7, 1},
    {"DATEDIFF", Reads::date, Gives::number, 8, 2},   {"DAYNAME", Reads::date, Gives::name, 9, 1},     {"MONTHNAME", Reads::date, Gives::name, 9, 1},
    {"LAST_DAY", Reads::date, Gives::date, 0, 1},     {"DATE", Reads::date, Gives::date, 0, 1},        {"HOUR", Reads::time, Gives::number, 3, 1},
    {"MINUTE", Reads::time, Gives::number, 2, 1},     {"SECOND", Reads::time, Gives::number, 2, 1},    {"MICROSECOND", Reads::time, Gives::number, 20, 1},
    {"TIME", Reads::time, Gives::time, 0, 1},
};

const DateFunction* named(const std::string& name) {
  for (const auto& f : date_functions)
    if (f.name == name) return &f;
  return nullptr;
}

// Days since 1970-01-01 of a date.
int64_t days_of(const Moment& m) {
  Moment day = m;
  day.hour = day.minute = day.second = day.micro = 0;
  day.negative = false;
  const int64_t us = temporal::to_unix_us(day);
  return us >= 0 ? us / day_us : -((-us + day_us - 1) / day_us);
}

// The fraction digits a TIME() result keeps: its argument's own, a string
// constant's written digits (observed: TIME('...56.5') has 1).
int time_digits(const Expr& arg, const ResultType& t) {
  if (is_temporal_type(t)) return temporal_kind_of(t) == Kind::date ? 0 : t.decimals;
  if (arg.kind == ExprKind::string) {
    const auto point = arg.value.rfind('.');
    if (point == std::string::npos) return 0;
    int n = 0;
    for (size_t i = point + 1; i < arg.value.size() && std::isdigit(static_cast<unsigned char>(arg.value[i])); ++i) ++n;
    return std::min(n, 6);
  }
  return t.decimals == any_decimals ? 6 : std::min<int>(t.decimals, 6);
}

}  // namespace

std::optional<ResultType> Evaluator::date_function(const Expr& e) const {
  const auto* fn = named(e.value);
  if (!fn) return std::nullopt;
  if (e.args.size() != fn->arguments) throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + e.value + "'");
  for (const auto& arg : e.args) type(*arg);
  // Every one may be NULL, a constant argument's too (observed).
  switch (fn->gives) {
    case Gives::year: {
      ResultType t;
      t.field_type = f::year;
      t.collation = protocol::collation::binary;
      t.length = 4;
      t.precision = 4;
      t.flags = c::unsigned_ | c::binary | c::num;
      return t;
    }
    case Gives::number:
      return integer_type(fn->digits, false, false);
    case Gives::name: {
      ResultType t;
      t.field_type = f::var_string;
      t.collation = _settings.result_collation;
      t.length = static_cast<uint32_t>(fn->digits) * _settings.result_max_bytes;
      t.decimals = any_decimals;
      t.flags = _settings.result_collation_binary ? c::binary : 0;
      return t;
    }
    case Gives::date:
      return temporal_result_type(Kind::date, 0, false);
    case Gives::time:
      return temporal_result_type(Kind::time, time_digits(*e.args[0], type(*e.args[0])), false);
  }
  return std::nullopt;
}

Value Evaluator::date_function_value(const Expr& e) {
  const auto t = *date_function(e);
  const auto* fn = named(e.value);
  // The statement's date, which a TIME takes to be a date (observed: YEAR
  // of a TIME is this year).
  const auto today = [&] {
    auto m = temporal::from_unix_us(_settings.statement_us + static_cast<int64_t>(_settings.zone_seconds) * 1000000);
    m.hour = m.minute = m.second = m.micro = 0;
    return m;
  };
  // An argument as a date and time, or nothing (NULL, with 1292 for a value
  // that is no date).
  const auto as_datetime = [&](const Expr& arg) -> std::optional<Moment> {
    const auto at = type(arg);
    const auto v = value(arg);
    if (is_null(v)) return std::nullopt;
    const auto* s = std::get_if<std::string>(&v);
    std::optional<Moment> m;
    if (s && is_temporal_type(at)) {
      const auto kind = temporal_kind_of(at);
      m = temporal::parse_text(*s, kind);
      if (kind == Kind::time) {
        const int64_t into = (static_cast<int64_t>(m->hour) * 3600 + m->minute * 60 + m->second) * 1000000 + m->micro;
        m = temporal::from_unix_us(temporal::to_unix_us(today()) + (m->negative ? -into : into));
      }
    } else if (s && !at.number_string) {
      const auto read = temporal::read_datetime(*s, 6);
      m = read.moment;
    } else {
      m = temporal::from_number(number_text(v), Kind::datetime, 6);
    }
    if (!m) _warnings.push_back({"Warning", er_truncated_wrong_value, "Incorrect datetime value: '" + to_text(v, at).value_or("") + "'"});
    return m;
  };
  // An argument as a time, or nothing.
  const auto as_time = [&](const Expr& arg) -> std::optional<Moment> {
    const auto at = type(arg);
    const auto v = value(arg);
    if (is_null(v)) return std::nullopt;
    const auto* s = std::get_if<std::string>(&v);
    std::optional<Moment> m;
    if (s && is_temporal_type(at)) {
      const auto kind = temporal_kind_of(at);
      m = kind == Kind::date ? Moment{} : temporal::parse_text(*s, kind);
    } else if (s && !at.number_string) {
      const auto read = temporal::read_time(*s, 6);
      m = read.moment;
      if (read.clamped) _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect time value: '" + *s + "'"});
    } else {
      m = temporal::from_number(number_text(v), Kind::time, 6);
    }
    if (!m) _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect time value: '" + to_text(v, at).value_or("") + "'"});
    if (m) m->year = m->month = m->day = 0;
    return m;
  };
  if (fn->reads == Reads::time) {
    const auto m = as_time(*e.args[0]);
    if (!m) return {};
    if (e.value == "HOUR") return static_cast<int64_t>(m->hour);
    if (e.value == "MINUTE") return static_cast<int64_t>(m->minute);
    if (e.value == "SECOND") return static_cast<int64_t>(m->second);
    if (e.value == "MICROSECOND") return static_cast<int64_t>(m->micro);
    auto rounded = *m;
    temporal::round(rounded, Kind::time, t.decimals);
    return temporal::text(rounded, Kind::time, t.decimals);
  }
  const auto m = as_datetime(*e.args[0]);
  if (!m) return {};
  const int64_t days = days_of(*m);
  if (e.value == "YEAR") return static_cast<int64_t>(m->year);
  if (e.value == "MONTH") return static_cast<int64_t>(m->month);
  if (e.value == "DAY" || e.value == "DAYOFMONTH") return static_cast<int64_t>(m->day);
  // 1970-01-01 was a Thursday.
  if (e.value == "DAYOFWEEK") return ((days + 4) % 7 + 7) % 7 + 1;
  if (e.value == "WEEKDAY") return ((days + 3) % 7 + 7) % 7;
  if (e.value == "DAYOFYEAR") return days - days_of(Moment{false, m->year, 1, 1, 0, 0, 0, 0}) + 1;
  if (e.value == "QUARTER") return static_cast<int64_t>((m->month + 2) / 3);
  if (e.value == "TO_DAYS") return days + days_to_epoch;
  if (e.value == "DATEDIFF") {
    const auto other = as_datetime(*e.args[1]);
    if (!other) return {};
    return days - days_of(*other);
  }
  if (e.value == "DAYNAME") {
    static constexpr std::array<std::string_view, 7> days_named = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    return std::string(days_named[static_cast<size_t>(((days + 3) % 7 + 7) % 7)]);
  }
  if (e.value == "MONTHNAME") {
    static constexpr std::array<std::string_view, 12> months = {"January", "February", "March",     "April",   "May",      "June",
                                                                "July",    "August",   "September", "October", "November", "December"};
    return std::string(months[m->month - 1]);
  }
  auto day = *m;
  if (e.value == "LAST_DAY") {
    // The day before the first of the next month.
    Moment next{false, m->month == 12 ? m->year + 1 : m->year, m->month == 12 ? 1u : m->month + 1, 1, 0, 0, 0, 0};
    day = temporal::from_unix_us(temporal::to_unix_us(next) - day_us);
  }
  return temporal::text(day, Kind::date, 0);
}

}  // namespace kvasir::exec
