//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Date arithmetic, typed and evaluated as MySQL 8.4.13 does (the manual's
// "Temporal Intervals" and "Date and Time Functions", as observed:
// TODO/2026-11-23-temporal-notes.md): DATE_ADD and DATE_SUB, which the
// parser makes of ADDDATE, SUBDATE, TIMESTAMPADD, + INTERVAL and - INTERVAL;
// TIMESTAMPDIFF; EXTRACT.

#include <algorithm>
#include <array>
#include <cstdint>
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
using temporal::Kind;
using temporal::Moment;

constexpr uint8_t any_decimals = protocol::not_fixed_decimals;
constexpr uint16_t er_datetime_function_overflow = 1441;
constexpr int64_t second_us = 1000000;
constexpr int64_t day_us = 86400 * second_us;
// The last day MySQL reckons, 9999-12-31, as TO_DAYS counts.
constexpr int64_t max_day_number = 3652424;
constexpr int64_t max_time_us = (838LL * 3600 + 59 * 60 + 59) * second_us;

// The fields a unit's amount fills, in order.
enum Field : uint8_t { in_year, in_month, in_day, in_hour, in_minute, in_second, in_micro };

struct Unit {
  std::string_view name;
  Field first, last;
  // EXTRACT's result length.
  uint32_t extract_length;
};

constexpr Unit units[] = {
    {"MICROSECOND", in_micro, in_micro, 7},
    {"SECOND", in_second, in_second, 3},
    {"MINUTE", in_minute, in_minute, 3},
    {"HOUR", in_hour, in_hour, 4},
    {"DAY", in_day, in_day, 3},
    {"WEEK", in_day, in_day, 3},
    {"MONTH", in_month, in_month, 3},
    {"QUARTER", in_month, in_month, 2},
    {"YEAR", in_year, in_year, 5},
    {"SECOND_MICROSECOND", in_second, in_micro, 9},
    {"MINUTE_MICROSECOND", in_minute, in_micro, 11},
    {"MINUTE_SECOND", in_minute, in_second, 5},
    {"HOUR_MICROSECOND", in_hour, in_micro, 14},
    {"HOUR_SECOND", in_hour, in_second, 8},
    {"HOUR_MINUTE", in_hour, in_minute, 6},
    {"DAY_MICROSECOND", in_day, in_micro, 20},
    {"DAY_SECOND", in_day, in_second, 13},
    {"DAY_MINUTE", in_day, in_minute, 11},
    {"DAY_HOUR", in_day, in_hour, 9},
    {"YEAR_MONTH", in_year, in_month, 7},
};

const Unit& unit_named(const std::string& name) {
  for (const auto& u : units)
    if (u.name == name) return u;
  return units[0];
}

// Days and longer: a DATE moved by one stays a DATE.
bool date_unit(const Unit& u) { return u.last <= in_day; }
// Hours and shorter: a TIME moved by one stays a TIME.
bool time_unit(const Unit& u) { return u.first >= in_hour; }

// The fraction digits an amount brings: six for a microsecond unit; a
// SECOND's number its own (a string six); none otherwise.
int amount_digits(const Unit& u, const ResultType& t) {
  if (u.last == in_micro) return 6;
  if (u.first != in_second || u.last != in_second || t.field_type == f::null) return 0;
  if (is_string_type(t) || t.decimals == any_decimals) return 6;
  return std::min<int>(t.decimals, 6);
}

// A moment's time of day in microseconds.
int64_t time_of_day(const Moment& m) { return ((static_cast<int64_t>(m.hour) * 60 + m.minute) * 60 + m.second) * second_us + m.micro; }

// MySQL's days in a month: year 0 is no leap year.
uint32_t month_days(uint32_t year, uint32_t month) { return year == 0 && month == 2 ? 28 : temporal::days_in_month(year, month); }

// An amount's fields, read as the unit's: a compound unit's string as
// digit groups split by anything else, filled from the right when short (a
// microsecond group scaled by its digits); nothing when it has too many.
struct Amount {
  bool negative = false;
  std::array<uint64_t, 7> fields{};
};

std::optional<Amount> compound_amount(std::string_view s, const Unit& u) {
  Amount out;
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  if (i < s.size() && s[i] == '-') {
    out.negative = true;
    ++i;
  }
  const auto digit = [&](size_t at) { return at < s.size() && s[at] >= '0' && s[at] <= '9'; };
  while (i < s.size() && !digit(i)) ++i;
  const size_t count = static_cast<size_t>(u.last - u.first) + 1;
  std::array<uint64_t, 7> values{};
  size_t last_digits = 0;
  size_t filled = count;
  for (size_t k = 0; k < count; ++k) {
    const size_t start = i;
    uint64_t value = 0;
    // Past 18 digits the amount is far beyond any date's range.
    for (; digit(i); ++i) value = i - start < 18 ? value * 10 + static_cast<uint64_t>(s[i] - '0') : value;
    last_digits = i - start;
    values[k] = value;
    while (i < s.size() && !digit(i)) ++i;
    if (i >= s.size() && k != count - 1) {
      filled = k + 1;
      break;
    }
  }
  if (i < s.size()) return std::nullopt;
  // Fewer groups than fields: they are the last fields.
  for (size_t k = 0; k < filled; ++k) out.fields[u.last - (filled - 1 - k)] = values[k];
  if (u.last == in_micro && last_digits < 6) {
    for (size_t k = last_digits; k < 6; ++k) out.fields[in_micro] *= 10;
  }
  return out;
}

}  // namespace

std::optional<ResultType> Evaluator::interval_function(const Expr& e) const {
  if (e.kind != sql::ExprKind::call) return std::nullopt;
  const bool add = e.value == "DATE_ADD" || e.value == "DATE_SUB";
  if (!add && e.value != "TIMESTAMPDIFF" && e.value != "EXTRACT") return std::nullopt;
  for (const auto& arg : e.args) type(*arg);
  const auto& u = unit_named(e.scope);
  if (e.value == "TIMESTAMPDIFF") return integer_type(20, false, false);
  if (e.value == "EXTRACT") return integer_type(static_cast<int>(u.extract_length) - 1, false, false);
  const auto operand = type(*e.args[0]);
  int fsp = amount_digits(u, type(*e.args[1]));
  if (is_temporal_type(operand)) {
    const auto kind = temporal_kind_of(operand);
    Kind gives = Kind::datetime;
    if (kind == Kind::date && date_unit(u)) gives = Kind::date;
    if (kind == Kind::time && time_unit(u)) gives = Kind::time;
    if (kind != Kind::date) fsp = std::max<int>(fsp, operand.decimals);
    return temporal_result_type(gives, gives == Kind::date ? 0 : fsp, false);
  }
  // Anything else gives a string, a date's or a date and time's text
  // (observed: STRING, length 29 characters).
  ResultType t;
  t.field_type = f::string;
  t.collation = _settings.result_collation;
  t.length = 29 * _settings.result_max_bytes;
  t.decimals = any_decimals;
  t.flags = _settings.result_collation_binary ? c::binary : 0;
  return t;
}

Value Evaluator::interval_function_value(const Expr& e) {
  const auto t = *interval_function(e);
  const auto& u = unit_named(e.scope);
  if (e.value == "TIMESTAMPDIFF") return timestamp_difference(e, u.name);
  if (e.value == "EXTRACT") return extracted(e, u.name);
  const auto overflow = [&](const char* field) {
    _warnings.push_back({"Warning", er_datetime_function_overflow, std::string("Datetime function: ") + field + " field overflow"});
    return Value{};
  };

  // The operand: a TIME that stays one, or a date and time.
  const auto operand_type = type(*e.args[0]);
  const bool stays_time = t.field_type == f::time;
  std::optional<Moment> m;
  bool has_time = true;
  if (stays_time) {
    const auto v = value(*e.args[0]);
    if (is_null(v)) return {};
    m = temporal::parse_text(std::get<std::string>(v), Kind::time);
  } else {
    m = datetime_argument(*e.args[0], &has_time);
    if (!m) return {};
  }

  // The amount, in the unit's fields.
  const auto& amount = *e.args[1];
  Amount a;
  if (u.first == u.last && u.first != in_second) {
    const auto n = integer_argument(amount);
    if (!n) return {};
    a.negative = *n < 0;
    a.fields[u.first] = a.negative ? 0 - static_cast<uint64_t>(*n) : static_cast<uint64_t>(*n);
    if (u.name == "WEEK") a.fields[in_day] = a.fields[in_day] > UINT64_MAX / 7 ? UINT64_MAX : a.fields[in_day] * 7;
    if (u.name == "QUARTER") a.fields[in_month] = a.fields[in_month] > UINT64_MAX / 3 ? UINT64_MAX : a.fields[in_month] * 3;
  } else {
    const auto at = type(amount);
    const auto v = value(amount);
    if (is_null(v)) return {};
    const auto* s = std::get_if<std::string>(&v);
    const std::string text = s && !at.number_string ? *s : number_text(v);
    if (u.first == in_second && u.last == in_second) {
      // SECOND: seconds and a fraction, as a decimal number.
      size_t i = 0;
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
      a.negative = i < text.size() && text[i] == '-';
      if (i < text.size() && (text[i] == '-' || text[i] == '+')) ++i;
      const size_t start = i;
      for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
        a.fields[in_second] = i - start < 18 ? a.fields[in_second] * 10 + static_cast<uint64_t>(text[i] - '0') : a.fields[in_second];
      if (i < text.size() && text[i] == '.') {
        uint64_t scale = 100000;
        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, scale /= 10) a.fields[in_micro] += static_cast<uint64_t>(text[i] - '0') * scale;
      }
      if (s && i != text.size()) _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect DECIMAL value: '" + text + "'"});
    } else {
      const auto read = compound_amount(text, u);
      if (!read) return overflow("date_add_interval");
      a = *read;
    }
  }
  if (e.value == "DATE_SUB") a.negative = !a.negative;
  const int64_t sign = a.negative ? -1 : 1;

  // Past these, any field alone moves a moment out of every range.
  static constexpr std::array<uint64_t, 7> limits = {20000, 240000, 4000000, 100000000, 6000000000ULL, 400000000000ULL, 400000000000000000ULL};
  for (size_t k = 0; k < limits.size(); ++k)
    if (a.fields[k] > limits[k]) return overflow(stays_time ? "time" : "datetime");
  const auto& fl = a.fields;
  const int64_t span_us =
      static_cast<int64_t>(fl[in_day]) * day_us +
      ((static_cast<int64_t>(fl[in_hour]) * 60 + static_cast<int64_t>(fl[in_minute])) * 60 + static_cast<int64_t>(fl[in_second])) * second_us +
      static_cast<int64_t>(fl[in_micro]);

  Moment r = *m;
  if (stays_time) {
    const int64_t total = (m->negative ? -1 : 1) * time_of_day(*m) + sign * span_us;
    if (std::llabs(total) > max_time_us) return overflow("time");
    const int64_t magnitude = std::llabs(total);
    r = Moment{};
    r.negative = total < 0;
    r.hour = static_cast<uint32_t>(magnitude / (3600 * second_us));
    r.minute = static_cast<uint32_t>(magnitude / (60 * second_us) % 60);
    r.second = static_cast<uint32_t>(magnitude / second_us % 60);
    r.micro = static_cast<uint32_t>(magnitude % second_us);
    return temporal::text(r, Kind::time, t.decimals);
  }
  if (u.name == "YEAR") {
    const int64_t year = static_cast<int64_t>(m->year) + sign * static_cast<int64_t>(fl[in_year]);
    if (year < 0 || year > 9999) return overflow("datetime");
    r.year = static_cast<uint32_t>(year);
    if (r.month == 2 && r.day == 29 && month_days(r.year, 2) == 28) r.day = 28;
  } else if (u.first <= in_month) {
    const int64_t period =
        static_cast<int64_t>(m->year) * 12 + m->month - 1 + sign * (static_cast<int64_t>(fl[in_year]) * 12 + static_cast<int64_t>(fl[in_month]));
    if (period < 0 || period >= 120000) return overflow("datetime");
    r.year = static_cast<uint32_t>(period / 12);
    r.month = static_cast<uint32_t>(period % 12 + 1);
    r.day = std::min(r.day, month_days(r.year, r.month));
  } else {
    // Days and time: counted from the first of the month, as MySQL does.
    const int64_t within = static_cast<int64_t>(m->day - 1) * day_us + time_of_day(*m) + sign * span_us;
    int64_t days = within / day_us;
    int64_t rest = within % day_us;
    if (rest < 0) {
      rest += day_us;
      --days;
    }
    const int64_t day = temporal::day_number(Moment{false, m->year, m->month, 1, 0, 0, 0, 0}) + days;
    if (day < 0 || day > max_day_number) return overflow("datetime");
    r = temporal::from_day_number(day);
    r.hour = static_cast<uint32_t>(rest / (3600 * second_us));
    r.minute = static_cast<uint32_t>(rest / (60 * second_us) % 60);
    r.second = static_cast<uint32_t>(rest / second_us % 60);
    r.micro = static_cast<uint32_t>(rest % second_us);
  }
  if (t.field_type == f::string) {
    // A string operand's text: a date when it was one moved by days or
    // longer, else a date and time, with six fraction digits once it has a
    // fraction.
    const bool date = !has_time && date_unit(u) && !is_temporal_type(operand_type);
    return temporal::text(r, date ? Kind::date : Kind::datetime, r.micro ? 6 : 0);
  }
  return temporal::text(r, temporal_kind_of(t), t.decimals);
}

Value Evaluator::timestamp_difference(const Expr& e, std::string_view unit) {
  const auto a = datetime_argument(*e.args[0]);
  if (!a) return {};
  const auto b = datetime_argument(*e.args[1]);
  if (!b) return {};
  const int64_t diff = temporal::to_unix_us(*b) - temporal::to_unix_us(*a);
  if (unit == "MICROSECOND") return diff;
  if (unit == "SECOND") return diff / second_us;
  if (unit == "MINUTE") return diff / (60 * second_us);
  if (unit == "HOUR") return diff / (3600 * second_us);
  if (unit == "DAY") return diff / day_us;
  if (unit == "WEEK") return diff / (7 * day_us);
  // Whole months from the earlier to the later: one fewer when the later's
  // day and time are before the earlier's.
  const bool backwards = diff < 0;
  const auto& lo = backwards ? *b : *a;
  const auto& hi = backwards ? *a : *b;
  int64_t months = (static_cast<int64_t>(hi.year) * 12 + hi.month) - (static_cast<int64_t>(lo.year) * 12 + lo.month);
  if (hi.day < lo.day || (hi.day == lo.day && time_of_day(hi) < time_of_day(lo))) --months;
  if (backwards) months = -months;
  if (unit == "QUARTER") return months / 3;
  if (unit == "YEAR") return months / 12;
  return months;
}

Value Evaluator::extracted(const Expr& e, std::string_view unit) {
  const auto& u = unit_named(e.scope);
  if (date_unit(u)) {
    const auto m = datetime_argument(*e.args[0]);
    if (!m) return {};
    if (unit == "YEAR") return static_cast<int64_t>(m->year);
    if (unit == "YEAR_MONTH") return static_cast<int64_t>(m->year) * 100 + m->month;
    if (unit == "QUARTER") return static_cast<int64_t>((m->month + 2) / 3);
    if (unit == "MONTH") return static_cast<int64_t>(m->month);
    if (unit == "DAY") return static_cast<int64_t>(m->day);
    // WEEK in mode 0: weeks begin on Sunday, and the days before the year's
    // first Sunday are week 0.
    const int64_t day = temporal::day_number(*m);
    const int64_t first = temporal::day_number(Moment{false, m->year, 1, 1, 0, 0, 0, 0});
    // The weekday from Sunday, as MySQL counts it from a day number.
    const int64_t weekday = (first + 6) % 7;
    const int64_t first_sunday = (7 - weekday) % 7;
    const int64_t into = day - first;
    return into < first_sunday ? int64_t{0} : (into - first_sunday) / 7 + 1;
  }
  const auto m = time_argument(*e.args[0], true);
  if (!m) return {};
  const int64_t hms = static_cast<int64_t>(m->hour) * 10000 + m->minute * 100 + m->second;
  int64_t out = 0;
  if (unit == "HOUR") out = m->hour;
  if (unit == "MINUTE") out = m->minute;
  if (unit == "SECOND") out = m->second;
  if (unit == "MICROSECOND") out = m->micro;
  if (unit == "DAY_HOUR") out = static_cast<int64_t>(m->day) * 100 + m->hour;
  if (unit == "DAY_MINUTE") out = static_cast<int64_t>(m->day) * 10000 + m->hour * 100 + m->minute;
  if (unit == "DAY_SECOND") out = static_cast<int64_t>(m->day) * 1000000 + hms;
  if (unit == "DAY_MICROSECOND") out = (static_cast<int64_t>(m->day) * 1000000 + hms) * second_us + m->micro;
  if (unit == "HOUR_MINUTE") out = static_cast<int64_t>(m->hour) * 100 + m->minute;
  if (unit == "HOUR_SECOND") out = hms;
  if (unit == "HOUR_MICROSECOND") out = hms * second_us + m->micro;
  if (unit == "MINUTE_SECOND") out = static_cast<int64_t>(m->minute) * 100 + m->second;
  if (unit == "MINUTE_MICROSECOND") out = (static_cast<int64_t>(m->minute) * 100 + m->second) * second_us + m->micro;
  if (unit == "SECOND_MICROSECOND") out = static_cast<int64_t>(m->second) * second_us + m->micro;
  return m->negative ? -out : out;
}

}  // namespace kvasir::exec
