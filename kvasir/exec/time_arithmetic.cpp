//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Arithmetic on times and Unix time, typed and evaluated as MySQL 8.4.13
// does (the manual's "Date and Time Functions", as observed:
// TODO/2026-11-23-temporal-notes.md): ADDTIME, SUBTIME, TIMEDIFF, MAKEDATE,
// MAKETIME, SEC_TO_TIME, TIME_TO_SEC, FROM_DAYS, TO_SECONDS,
// UNIX_TIMESTAMP and FROM_UNIXTIME.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
constexpr int64_t second_us = 1000000;
constexpr int64_t max_time_us = (838LL * 3600 + 59 * 60 + 59) * second_us;
constexpr int64_t max_day_number = 3652424;
// The last second UNIX_TIMESTAMP and FROM_UNIXTIME reckon: 3001-01-18
// 23:59:59 UTC (observed).
constexpr int64_t max_unix_timestamp = 32536771199;
constexpr int64_t max_from_unixtime = max_unix_timestamp;

struct Arity {
  std::string_view name;
  size_t least, most;
};

constexpr Arity time_arithmetic_functions[] = {
    {"ADDTIME", 2, 2},     {"SUBTIME", 2, 2},   {"TIMEDIFF", 2, 2},   {"MAKEDATE", 2, 2},       {"MAKETIME", 3, 3},      {"SEC_TO_TIME", 1, 1},
    {"TIME_TO_SEC", 1, 1}, {"FROM_DAYS", 1, 1}, {"TO_SECONDS", 1, 1}, {"UNIX_TIMESTAMP", 0, 1}, {"FROM_UNIXTIME", 1, 2},
};

const Arity* arithmetic_named(const std::string& name) {
  for (const auto& f : time_arithmetic_functions)
    if (f.name == name) return &f;
  return nullptr;
}

// A time's signed microseconds; and back.
int64_t time_us(const Moment& m) {
  const int64_t us = ((static_cast<int64_t>(m.hour) * 60 + m.minute) * 60 + m.second) * second_us + m.micro;
  return m.negative ? -us : us;
}

Moment time_of(int64_t us) {
  Moment m;
  m.negative = us < 0;
  const int64_t magnitude = std::llabs(us);
  m.hour = static_cast<uint32_t>(magnitude / (3600 * second_us));
  m.minute = static_cast<uint32_t>(magnitude / (60 * second_us) % 60);
  m.second = static_cast<uint32_t>(magnitude / second_us % 60);
  m.micro = static_cast<uint32_t>(magnitude % second_us);
  return m;
}

// A literal the statement holds, never NULL: a string or a number, or a
// temporal literal (observed: UNIX_TIMESTAMP('x') and FROM_DAYS(1) are
// NOT_NULL, over a CAST nullable).
bool literal(const Expr& e) {
  return e.kind == ExprKind::string || e.kind == ExprKind::integer || e.kind == ExprKind::decimal || e.kind == ExprKind::floating ||
         (e.kind == ExprKind::call && e.value == "TEMPORAL");
}

// A number's text as seconds and microseconds rounded to `fsp` digits;
// nothing for text that is no number.
std::optional<std::pair<int64_t, int64_t>> seconds_of(std::string_view s, int fsp) {
  size_t i = 0;
  while (i < s.size() && s[i] == ' ') ++i;
  const bool negative = i < s.size() && s[i] == '-';
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
  const size_t start = i;
  int64_t whole = 0;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) whole = whole < 100000000000000LL ? whole * 10 + (s[i] - '0') : whole;
  int64_t micro = 0;
  int seen = 0;
  bool seventh_up = false;
  if (i < s.size() && s[i] == '.') {
    for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++seen) {
      if (seen < 6) micro = micro * 10 + (s[i] - '0');
      if (seen == 6) seventh_up = s[i] >= '5';
    }
  }
  if (i == start) return std::nullopt;
  for (int k = std::min(seen, 6); k < 6; ++k) micro *= 10;
  // To the result's digits, half up.
  if (fsp >= 6) {
    micro += seventh_up ? 1 : 0;
  } else {
    int64_t unit = 1;
    for (int k = fsp; k < 6; ++k) unit *= 10;
    micro = (micro + unit / 2) / unit * unit;
  }
  if (micro >= second_us) {
    micro -= second_us;
    ++whole;
  }
  return std::pair{negative ? -whole : whole, negative ? -micro : micro};
}

}  // namespace

std::optional<std::pair<Moment, Kind>> Evaluator::temporal_operand(const Expr& arg) {
  const auto at = type(arg);
  const auto v = value(arg);
  if (is_null(v)) return std::nullopt;
  const auto* s = std::get_if<std::string>(&v);
  if (s && is_temporal_type(at)) {
    const auto kind = temporal_kind_of(at);
    return std::pair{temporal::parse_text(*s, kind), kind};
  }
  if (s && !at.number_string) {
    const auto whole = temporal::read_datetime(*s, 6);
    if (whole.moment && whole.has_time) return std::pair{*whole.moment, Kind::datetime};
    const auto read = temporal::read_time(*s, 6);
    if (read.moment && !read.clamped) return std::pair{*read.moment, Kind::time};
    _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect time value: '" + *s + "'"});
    if (read.moment) return std::pair{*read.moment, Kind::time};
    return std::nullopt;
  }
  const auto n = number_text(v);
  if (n.substr(0, n.find('.')).size() > 8) {
    if (const auto m = temporal::from_number(n, Kind::datetime, 6)) return std::pair{*m, Kind::datetime};
  } else if (const auto m = temporal::from_number(n, Kind::time, 6)) {
    return std::pair{*m, Kind::time};
  }
  _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect time value: '" + n + "'"});
  return std::nullopt;
}

std::optional<ResultType> Evaluator::time_arithmetic(const Expr& e) const {
  if (e.kind != ExprKind::call) return std::nullopt;
  const auto* fn = arithmetic_named(e.value);
  if (!fn) return std::nullopt;
  if (e.args.size() < fn->least || e.args.size() > fn->most)
    throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + e.value + "'");
  for (const auto& arg : e.args) type(*arg);
  const auto digits = [&](size_t i) { return fraction_digits(*e.args[i], type(*e.args[i])); };
  const auto& name = e.value;
  if (name == "TIME_TO_SEC") return integer_type(9, false, false);
  if (name == "TO_SECONDS") return integer_type(20, false, false);
  if (name == "FROM_DAYS") return temporal_result_type(Kind::date, 0, literal(*e.args[0]));
  if (name == "MAKEDATE") return temporal_result_type(Kind::date, 0, false);
  // A string of seconds brings six digits (observed: SEC_TO_TIME('3661')).
  const auto seconds_digits = [&](size_t i) { return is_string_type(type(*e.args[i])) ? 6 : digits(i); };
  if (name == "MAKETIME") return temporal_result_type(Kind::time, seconds_digits(2), false);
  if (name == "SEC_TO_TIME") return temporal_result_type(Kind::time, seconds_digits(0), false);
  if (name == "TIMEDIFF") return temporal_result_type(Kind::time, std::max(digits(0), digits(1)), false);
  if (name == "UNIX_TIMESTAMP") {
    if (e.args.empty()) return integer_type(20, false, true);
    const auto& arg = *e.args[0];
    int fsp = digits(0);
    // A constant string's digits as it reads; one that is no date six.
    if (arg.kind == ExprKind::string) {
      const auto read = temporal::read_datetime(arg.value, 6);
      if (!read.moment) fsp = 6;
    }
    const bool not_null = literal(arg);
    if (!fsp) return integer_type(20, false, not_null);
    return decimal_type(11 + fsp, fsp, not_null);
  }
  if (name == "FROM_UNIXTIME") {
    if (e.args.size() == 2) {
      ResultType t;
      t.field_type = f::var_string;
      t.collation = _settings.result_collation;
      t.decimals = any_decimals;
      t.flags = _settings.result_collation_binary ? c::binary : 0;
      const auto& fmt = *e.args[1];
      t.length = (fmt.kind == ExprKind::string ? temporal::format_length(fmt.value) : 0) * _settings.result_max_bytes;
      return t;
    }
    const auto at = type(*e.args[0]);
    const int fsp = is_string_type(at) ? 6 : digits(0);
    return temporal_result_type(Kind::datetime, fsp, false);
  }
  // ADDTIME and SUBTIME: by the first argument's type.
  const auto a = type(*e.args[0]);
  const int fsp = std::max(digits(0), digits(1));
  if (is_temporal_type(a)) {
    const auto kind = temporal_kind_of(a);
    return temporal_result_type(kind == Kind::time ? Kind::time : Kind::datetime, fsp, false);
  }
  ResultType t;
  t.field_type = f::string;
  t.collation = _settings.result_collation;
  t.length = 29 * _settings.result_max_bytes;
  t.decimals = any_decimals;
  t.flags = _settings.result_collation_binary ? c::binary : 0;
  return t;
}

Value Evaluator::time_arithmetic_value(const Expr& e) {
  const auto t = *time_arithmetic(e);
  const auto& name = e.value;
  // A TIME result within 838:59:59, clamped with 1292 naming the value.
  const auto clamped_time = [&](int64_t us, int fsp) -> Value {
    if (std::llabs(us) > max_time_us) {
      _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect time value: '" + temporal::text(time_of(us), Kind::time, fsp) + "'"});
      us = us < 0 ? -max_time_us : max_time_us;
    }
    return temporal::text(time_of(us), Kind::time, fsp);
  };

  if (name == "ADDTIME" || name == "SUBTIME") {
    const auto x = temporal_operand(*e.args[0]);
    if (!x) return {};
    const auto y = time_argument(*e.args[1]);
    if (!y) return {};
    const int64_t add = (name == "SUBTIME" ? -1 : 1) * time_us(*y);
    const bool as_string = t.field_type == f::string;
    if (x->second != Kind::time) {
      const auto moved = temporal::from_unix_us(temporal::to_unix_us(x->first) + add);
      if (moved.year < 1 || moved.year > 9999) return {};
      if (as_string) return temporal::text(moved, Kind::datetime, moved.micro ? 6 : 0);
      return temporal::text(moved, Kind::datetime, t.decimals);
    }
    const int64_t total = time_us(x->first) + add;
    return clamped_time(total, as_string ? (total % second_us ? 6 : 0) : t.decimals);
  }
  if (name == "TIMEDIFF") {
    const auto x = temporal_operand(*e.args[0]);
    if (!x) return {};
    const auto y = temporal_operand(*e.args[1]);
    if (!y) return {};
    // Values of two kinds have no difference (observed: a DATETIME and a
    // DATE, or a TIMESTAMP and a TIME, are NULL).
    if (x->second != y->second) return {};
    const int64_t diff = x->second != Kind::time ? temporal::to_unix_us(x->first) - temporal::to_unix_us(y->first) : time_us(x->first) - time_us(y->first);
    return clamped_time(diff, t.decimals);
  }
  if (name == "TIME_TO_SEC") {
    const auto m = time_argument(*e.args[0]);
    if (!m) return {};
    return time_us(*m) / second_us;
  }
  if (name == "TO_SECONDS") {
    const auto m = datetime_argument(*e.args[0]);
    if (!m) return {};
    return temporal::day_number(*m) * 86400 + (static_cast<int64_t>(m->hour) * 60 + m->minute) * 60 + m->second;
  }
  if (name == "FROM_DAYS") {
    const auto n = integer_argument(*e.args[0]);
    if (!n) return {};
    if (*n > max_day_number) return {};
    return temporal::text(temporal::from_day_number(*n), Kind::date, 0);
  }
  if (name == "MAKEDATE") {
    const auto y = integer_argument(*e.args[0]);
    const auto d = integer_argument(*e.args[1]);
    if (!y || !d || *d <= 0 || *y < 0 || *y > 9999) return {};
    const int64_t year = *y < 70 ? *y + 2000 : *y < 100 ? *y + 1900 : *y;
    const int64_t day = temporal::day_number(Moment{false, static_cast<uint32_t>(year), 1, 1, 0, 0, 0, 0}) + *d - 1;
    if (day > max_day_number) return {};
    return temporal::text(temporal::from_day_number(day), Kind::date, 0);
  }
  if (name == "MAKETIME" || name == "SEC_TO_TIME") {
    const auto& last = *e.args.back();
    const auto lv = value(last);
    if (is_null(lv)) return {};
    const auto text = number_text(lv);
    const auto* s = std::get_if<std::string>(&lv);
    const auto seconds = seconds_of(s && !type(last).number_string ? *s : text, t.decimals);
    if (!seconds) return {};
    if (name == "SEC_TO_TIME") {
      const int64_t us = seconds->first * second_us + seconds->second;
      if (std::llabs(us) > max_time_us) {
        _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect time value: '" + (s ? *s : text) + "'"});
        return temporal::text(time_of(us < 0 ? -max_time_us : max_time_us), Kind::time, t.decimals);
      }
      return temporal::text(time_of(us), Kind::time, t.decimals);
    }
    const auto h = integer_argument(*e.args[0]);
    const auto m = integer_argument(*e.args[1]);
    if (!h || !m || *m < 0 || *m > 59 || seconds->first < 0 || seconds->first > 59 || seconds->second < 0) return {};
    const int64_t magnitude = ((std::llabs(*h) * 60 + *m) * 60 + seconds->first) * second_us + seconds->second;
    return clamped_time(*h < 0 ? -magnitude : magnitude, t.decimals);
  }
  if (name == "UNIX_TIMESTAMP") {
    const int64_t zone_us = static_cast<int64_t>(_settings.zone_seconds) * second_us;
    if (e.args.empty()) return _settings.statement_us / second_us;
    const auto& arg = *e.args[0];
    if (is_null(value(arg))) return {};
    const auto m = datetime_argument(arg);
    int64_t us = m ? temporal::to_unix_us(*m) - zone_us : 0;
    if (us < 0 || us / second_us > max_unix_timestamp) us = 0;
    if (t.field_type == f::longlong) return us / second_us;
    char digits[8];
    std::snprintf(digits, sizeof digits, "%06lld", static_cast<long long>(us % second_us));
    return *Decimal::parse(std::to_string(us / second_us) + "." + std::string(digits, static_cast<size_t>(t.decimals)));
  }
  // FROM_UNIXTIME.
  const auto& arg = *e.args[0];
  const auto v = value(arg);
  if (is_null(v)) return {};
  const auto* s = std::get_if<std::string>(&v);
  const int fsp = e.args.size() == 2 ? 6 : t.decimals;
  const auto seconds = seconds_of(s && !type(arg).number_string ? *s : number_text(v), fsp);
  if (!seconds || seconds->first < 0 || seconds->second < 0 || seconds->first > max_from_unixtime) return {};
  const auto m = temporal::from_unix_us(seconds->first * second_us + seconds->second + static_cast<int64_t>(_settings.zone_seconds) * second_us);
  if (e.args.size() == 2) {
    const auto fv = value(*e.args[1]);
    if (is_null(fv)) return {};
    const auto out = temporal::format(m, to_text(fv, type(*e.args[1])).value_or(""), false);
    if (!out) return {};
    return *out;
  }
  return temporal::text(m, Kind::datetime, t.decimals);
}

}  // namespace kvasir::exec
