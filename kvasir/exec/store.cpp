//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/store.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>

#include "dannan/encoding/base.h"
#include "kvasir/exec/expression.h"
#include "kvasir/exec/temporal.h"
#include "kvasir/protocol/types.h"

namespace kvasir::exec {

namespace {

using catalog::TypeKind;
using Result = Stored::Result;

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

enum class Parse : uint8_t { ok, not_a_number, trailing, too_big };

// The digits a DECIMAL column can hold before the point, and a little more to
// round from after it.
constexpr size_t most_whole_digits = 65;
constexpr size_t most_fraction_digits = 31;

// [sign]digits[.digits][e[sign]digits], the whole of `text`, as an exact
// decimal. Digits past the 31st after the point are dropped; `lost` says
// whether any of them was not 0.
Parse exact_decimal(std::string_view text, Decimal& out, bool& lost) {
  size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) negative = text[i++] == '-';
  std::string digits;
  long point = -1;
  for (; i < text.size(); ++i) {
    if (is_digit(text[i])) {
      digits.push_back(text[i]);
    } else if (text[i] == '.' && point < 0) {
      point = static_cast<long>(digits.size());
    } else {
      break;
    }
  }
  if (digits.empty()) return Parse::not_a_number;
  if (point < 0) point = static_cast<long>(digits.size());
  long exponent = 0;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    bool exponent_negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) exponent_negative = text[i++] == '-';
    const size_t start = i;
    for (; i < text.size() && is_digit(text[i]); ++i) exponent = std::min(exponent * 10 + (text[i] - '0'), 100000L);
    if (i == start) return Parse::trailing;
    if (exponent_negative) exponent = -exponent;
  }
  if (i != text.size()) return Parse::trailing;
  lost = false;
  if (digits.find_first_not_of('0') == std::string::npos) {
    out = Decimal();
    return Parse::ok;
  }
  // Leading zeros carry nothing.
  const size_t first = digits.find_first_not_of('0');
  digits.erase(0, first);
  point -= static_cast<long>(first);
  const long whole_length = point + exponent;
  if (whole_length > static_cast<long>(most_whole_digits)) return Parse::too_big;
  std::string whole, fraction;
  if (whole_length <= 0) {
    if (-whole_length >= static_cast<long>(most_fraction_digits)) {
      out = Decimal();
      lost = true;
      return Parse::ok;
    }
    whole = "0";
    fraction = std::string(static_cast<size_t>(-whole_length), '0') + digits;
  } else if (whole_length >= static_cast<long>(digits.size())) {
    whole = digits + std::string(static_cast<size_t>(whole_length) - digits.size(), '0');
  } else {
    whole = digits.substr(0, static_cast<size_t>(whole_length));
    fraction = digits.substr(static_cast<size_t>(whole_length));
  }
  if (fraction.size() > most_fraction_digits) {
    lost = fraction.find_first_not_of('0', most_fraction_digits) != std::string::npos;
    fraction.resize(most_fraction_digits);
  }
  out = *Decimal::parse((negative ? "-" : "") + whole + (fraction.empty() ? "" : "." + fraction));
  return Parse::ok;
}

// The longest leading part of `text`, blanks skipped, that reads as a number:
// [sign]digits[.digits][e[sign]digits]; empty when there is none.
std::string_view leading_number(std::string_view text) {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  size_t i = 0;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
  size_t digits = 0;
  for (; i < text.size() && is_digit(text[i]); ++i) ++digits;
  if (i < text.size() && text[i] == '.') {
    size_t j = i + 1;
    for (; j < text.size() && is_digit(text[j]); ++j) ++digits;
    if (digits > 0) i = j;
  }
  if (digits == 0) return {};
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    size_t j = i + 1;
    if (j < text.size() && (text[j] == '+' || text[j] == '-')) ++j;
    size_t k = j;
    for (; k < text.size() && is_digit(text[k]); ++k) {
    }
    if (k > j) i = k;
  }
  return text.substr(0, i);
}

// Whether a value that is too big to read is negative.
bool negative_text(const Value& value) {
  const auto* s = std::get_if<std::string>(&value);
  if (!s) return false;
  const auto t = trim(*s);
  return !t.empty() && t.front() == '-';
}

// 0 with `scale` places.
std::string zero_text(int scale) { return scale > 0 ? "0." + std::string(static_cast<size_t>(scale), '0') : "0"; }

// A binary string as a number: an unsigned integer, big-endian.
std::optional<uint64_t> bytes_number(const std::string& s) {
  if (s.size() > 8) return std::nullopt;
  uint64_t out = 0;
  for (const char ch : s) out = (out << 8) | static_cast<uint8_t>(ch);
  return out;
}

// The value as an exact decimal, for an integer or DECIMAL column.
Parse as_exact(const Value& value, const ResultType& from, Decimal& out, bool& lost) {
  lost = false;
  if (const auto* i = std::get_if<int64_t>(&value)) {
    out = Decimal::from_int(*i);
  } else if (const auto* u = std::get_if<uint64_t>(&value)) {
    out = Decimal::from_uint(*u);
  } else if (const auto* d = std::get_if<Decimal>(&value)) {
    out = *d;
  } else if (const auto* r = std::get_if<double>(&value)) {
    return exact_decimal(format_double(*r), out, lost);
  } else if (const auto* s = std::get_if<std::string>(&value)) {
    if (from.number_string) {
      const auto n = bytes_number(*s);
      if (!n) return Parse::too_big;
      out = Decimal::from_uint(*n);
    } else {
      return exact_decimal(trim(*s), out, lost);
    }
  }
  return Parse::ok;
}

Result failure(Parse p) {
  if (p == Parse::too_big) return Result::out_of_range;
  return p == Parse::trailing ? Result::garbage : Result::invalid;
}

// The digits before the point.
size_t whole_digits(const Decimal& d) {
  auto text = d.truncated(0).to_string();
  if (!text.empty() && text[0] == '-') text.erase(0, 1);
  return text == "0" ? 0 : text.size();
}

Stored store_integer(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
  Value v = value;
  // A double rounds to even, as rint() does.
  if (const auto* r = std::get_if<double>(&value)) v = std::rint(*r);
  int64_t least = 0;
  uint64_t most = 0;
  switch (type.kind) {
    case TypeKind::tinyint:
      least = type.is_unsigned ? 0 : -128;
      most = type.is_unsigned ? 255 : 127;
      break;
    case TypeKind::smallint:
      least = type.is_unsigned ? 0 : -32768;
      most = type.is_unsigned ? 65535 : 32767;
      break;
    case TypeKind::mediumint:
      least = type.is_unsigned ? 0 : -8388608;
      most = type.is_unsigned ? 16777215 : 8388607;
      break;
    case TypeKind::int_:
      least = type.is_unsigned ? 0 : std::numeric_limits<int32_t>::min();
      most = type.is_unsigned ? std::numeric_limits<uint32_t>::max() : static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
      break;
    default:
      least = type.is_unsigned ? 0 : std::numeric_limits<int64_t>::min();
      most = type.is_unsigned ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
      break;
  }
  const auto low = Decimal::from_int(least), high = Decimal::from_uint(most);
  const auto clamped = [&](const Decimal& r) { return compare(r, low) < 0 ? low.to_string() : compare(r, high) > 0 ? high.to_string() : r.to_string(); };
  Decimal x;
  bool lost = false;
  if (const auto p = as_exact(v, from, x, lost); p != Parse::ok) {
    Stored s{failure(p), {}, "0"};
    if (p == Parse::too_big) {
      s.lenient = negative_text(value) ? low.to_string() : high.to_string();
    } else if (p == Parse::trailing) {
      Decimal y;
      const auto prefix = leading_number(std::get<std::string>(value));
      const auto q = exact_decimal(prefix, y, lost);
      if (q == Parse::ok) s.lenient = clamped(y.rounded(0));
      if (q == Parse::too_big) s.lenient = !prefix.empty() && prefix.front() == '-' ? low.to_string() : high.to_string();
    }
    return s;
  }
  const auto r = x.rounded(0);
  if (compare(r, low) < 0 || compare(r, high) > 0) return {Result::out_of_range, {}, clamped(r)};
  return {Result::stored, r.to_string(), {}};
}

Stored store_decimal(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
  // Past the range: the largest the column holds, of the value's sign; 0 for
  // a negative in an unsigned column.
  const auto limit = [&](bool negative) {
    if (negative && type.is_unsigned) return zero_text(type.scale);
    const auto whole = static_cast<size_t>(type.length - type.scale);
    std::string text = (negative ? "-" : "") + (whole ? std::string(whole, '9') : "0");
    if (type.scale > 0) text += "." + std::string(static_cast<size_t>(type.scale), '9');
    return text;
  };
  // Rounded to the scale, or nothing past the range.
  const auto fitted = [&](const Decimal& x) -> std::optional<Decimal> {
    const auto r = x.rounded(type.scale);
    if ((type.is_unsigned && r.negative()) || whole_digits(r) > static_cast<size_t>(type.length - type.scale)) return std::nullopt;
    return r;
  };
  Decimal x;
  bool lost = false;
  // A DECIMAL column calls any string it cannot read invalid in strict mode
  // (observed); one with a number before more text is garbage, its leading
  // number kept when errors are ignored.
  if (const auto p = as_exact(value, from, x, lost); p != Parse::ok) {
    if (p == Parse::too_big) return {Result::out_of_range, {}, limit(negative_text(value))};
    Stored s{p == Parse::trailing ? Result::garbage : Result::invalid, {}, zero_text(type.scale)};
    Decimal y;
    if (p == Parse::trailing && exact_decimal(leading_number(std::get<std::string>(value)), y, lost) == Parse::ok) {
      const auto r = fitted(y);
      s.lenient = r ? r->to_string() : limit(y.negative());
    }
    return s;
  }
  const auto r = fitted(x);
  if (!r) return {Result::out_of_range, {}, limit(x.negative())};
  return {lost || compare(x, *r) != 0 ? Result::truncated : Result::stored, r->to_string(), {}};
}

// Whether the whole text is a number as MySQL reads one.
bool numeric(std::string_view text) {
  Decimal unused;
  bool lost = false;
  const auto p = exact_decimal(text, unused, lost);
  return p == Parse::ok || p == Parse::too_big;
}

// A FLOAT as MySQL shows it: the fewest digits that read back as the same
// float.
std::string format_float(float f) {
  for (int digits = 1; digits <= 9; ++digits) {
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.*g", digits, static_cast<double>(f));
    if (std::strtof(buffer, nullptr) == f) return format_double(std::strtod(buffer, nullptr));
  }
  return format_double(f);
}

// The value as a FLOAT or DOUBLE column holds it, or why it cannot be.
Stored store_real_strict(const catalog::ColumnType& type, const Value& value, const ResultType& from);

Stored store_real(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
  const bool single = type.kind == TypeKind::float_;
  const auto shown = [&](double x) { return single ? format_float(static_cast<float>(x)) : format_double(x); };
  // Past the range: the largest of the value's sign; 0 for a negative in an
  // unsigned column.
  const auto limit = [&](bool negative) {
    if (negative && type.is_unsigned) return std::string("0");
    double most = single ? FLT_MAX : DBL_MAX;
    if (type.length > 0) most = std::pow(10.0, static_cast<int>(type.length) - type.scale) - std::pow(10.0, -type.scale);
    return shown(negative ? -most : most);
  };
  Stored s = store_real_strict(type, value, from);
  if (s.result == Result::out_of_range) {
    double d = 0;
    if (const auto* i = std::get_if<int64_t>(&value)) d = static_cast<double>(*i);
    if (const auto* x = std::get_if<Decimal>(&value)) d = x->to_double();
    if (const auto* r = std::get_if<double>(&value)) d = *r;
    if (const auto* t = std::get_if<std::string>(&value); t && !from.number_string) d = std::strtod(std::string(trim(*t)).c_str(), nullptr);
    s.lenient = limit(d < 0 || negative_text(value));
  } else if (s.result == Result::garbage) {
    // Its leading number, or 0.
    const std::string prefix(leading_number(std::get<std::string>(value)));
    const double d = prefix.empty() ? 0 : std::strtod(prefix.c_str(), nullptr);
    const auto fitted = store_real_strict(type, prefix.empty() ? Value{0.0} : Value{d}, ResultType{});
    s.lenient = fitted.result == Result::stored ? fitted.text : limit(d < 0);
  }
  return s;
}

Stored store_real_strict(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
  double d = 0;
  if (const auto* i = std::get_if<int64_t>(&value)) {
    d = static_cast<double>(*i);
  } else if (const auto* u = std::get_if<uint64_t>(&value)) {
    d = static_cast<double>(*u);
  } else if (const auto* x = std::get_if<Decimal>(&value)) {
    d = x->to_double();
  } else if (const auto* r = std::get_if<double>(&value)) {
    d = *r;
  } else if (const auto* s = std::get_if<std::string>(&value)) {
    if (from.number_string) {
      const auto n = bytes_number(*s);
      if (!n) return {Result::out_of_range, {}, {}};
      d = static_cast<double>(*n);
    } else {
      const std::string text(trim(*s));
      // A DOUBLE or FLOAT column calls any string it cannot read truncated
      // (observed).
      if (!numeric(text)) return {Result::garbage, {}, {}};
      d = std::strtod(text.c_str(), nullptr);
    }
  }
  if (!std::isfinite(d)) return {Result::out_of_range, {}, {}};
  if (type.length > 0) {
    // FLOAT(M,D) and DOUBLE(M,D): rounded to D, and below 10^(M-D).
    const double scale = std::pow(10.0, type.scale);
    d = std::round(d * scale) / scale;
    if (std::fabs(d) >= std::pow(10.0, static_cast<int>(type.length) - type.scale)) return {Result::out_of_range, {}, {}};
  }
  if (type.kind == TypeKind::float_ && std::fabs(d) > FLT_MAX) return {Result::out_of_range, {}, {}};
  if (type.is_unsigned && d < 0) return {Result::out_of_range, {}, {}};
  return {Result::stored, type.kind == TypeKind::float_ ? format_float(static_cast<float>(d)) : format_double(d), {}};
}

// Well-formed UTF-8 (Dannan's RFC 3629 check), with no character past the
// BMP for utf8mb3.
bool valid_utf8(const std::string& s, bool three_bytes_at_most) {
  for (size_t i = 0; i < s.size();) {
    const size_t length = dannan::encoding::utf8_length(s, i);
    if (length == 0 || (length == 4 && three_bytes_at_most)) return false;
    i += length;
  }
  return true;
}

std::string text_of(const Value& value) {
  if (const auto* s = std::get_if<std::string>(&value)) return *s;
  if (const auto* i = std::get_if<int64_t>(&value)) return std::to_string(*i);
  if (const auto* u = std::get_if<uint64_t>(&value)) return std::to_string(*u);
  if (const auto* d = std::get_if<Decimal>(&value)) return d->to_string();
  if (const auto* r = std::get_if<double>(&value)) return format_double(*r);
  return {};
}

Stored store_string(const catalog::ColumnType& type, const Value& value) {
  std::string text = text_of(value);
  const bool characters = catalog::is_text(type.kind);
  const bool fixed = type.kind == TypeKind::char_ || type.kind == TypeKind::varchar || type.kind == TypeKind::binary || type.kind == TypeKind::varbinary;
  const uint64_t limit = fixed ? type.length : catalog::blob_max_bytes(type.kind);
  // As the column holds it: CHAR and VARCHAR count characters, the rest
  // bytes; CHAR is sent without its trailing spaces, BINARY filled with zero
  // bytes.
  const auto end_of = [&](const std::string& t) { return characters && fixed ? character_end(t, limit) : std::min<size_t>(t.size(), limit); };
  const auto finished = [&](std::string t) {
    if (type.kind == TypeKind::char_) {
      while (!t.empty() && t.back() == ' ') t.pop_back();
    } else if (type.kind == TypeKind::binary) {
      t.resize(type.length, '\0');
    }
    return t;
  };
  if (characters && !valid_utf8(text, type.charset == "utf8mb3")) {
    // Ignoring the error keeps what comes before the first bad byte.
    size_t good = 0;
    while (good < text.size()) {
      const size_t length = dannan::encoding::utf8_length(text, good);
      if (length == 0 || (length == 4 && type.charset == "utf8mb3")) break;
      good += length;
    }
    std::string kept = text.substr(0, good);
    kept.resize(end_of(kept));
    return {Result::invalid, {}, finished(kept)};
  }
  const size_t end = end_of(text);
  Result result = Result::stored;
  if (end < text.size()) {
    if (text.find_first_not_of(' ', end) != std::string::npos) return {Result::too_long, {}, finished(text.substr(0, end))};
    text.resize(end);
    if (type.kind != TypeKind::char_) result = Result::truncated;
  }
  return {result, finished(text), {}};
}

temporal::Kind temporal_kind(TypeKind kind) {
  if (kind == TypeKind::date) return temporal::Kind::date;
  if (kind == TypeKind::time) return temporal::Kind::time;
  return temporal::Kind::datetime;
}

// The zero value of a DATE, TIME or DATETIME column, what a statement that
// ignores errors stores for one it cannot take (observed: 0000-00-00).
std::string zero_temporal(const catalog::ColumnType& type) {
  temporal::Moment zero;
  return temporal::text(zero, temporal_kind(type.kind), type.scale);
}

// A value into a DATE, TIME, DATETIME or TIMESTAMP column (observed on MySQL
// 8.4.13, TODO/2026-11-23-temporal-notes.md): a string or number as CAST
// reads it, but trailing text or a TIME past 838:59:59 is invalid; a
// DATETIME's time dropped into a DATE is truncated (Note 1292); a
// TIMESTAMP outside 1970-01-01 00:00:01 to 2038-01-19 03:14:07 UTC is
// invalid.
Stored store_temporal(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
  const auto kind = temporal_kind(type.kind);
  const int fsp = type.scale;
  const auto invalid = [&] { return Stored{Result::invalid, {}, zero_temporal(type)}; };
  std::optional<temporal::Moment> m;
  const auto* s = std::get_if<std::string>(&value);
  if (s && is_temporal_type(from)) {
    const auto from_kind = from.field_type == protocol::field_type::date   ? temporal::Kind::date
                           : from.field_type == protocol::field_type::time ? temporal::Kind::time
                                                                           : temporal::Kind::datetime;
    // A TIME takes today's date to be a DATE, which the clock gives later.
    if (from_kind == temporal::Kind::time && kind != temporal::Kind::time) return invalid();
    m = temporal::parse_text(*s, from_kind);
    if (kind == temporal::Kind::time) m->year = m->month = m->day = 0;
  } else if (s && !from.number_string) {
    const auto read = kind == temporal::Kind::time ? temporal::read_time(*s, fsp) : temporal::read_datetime(*s, kind == temporal::Kind::date ? 6 : fsp);
    // A TIME past its limit is kept clamped by a statement that ignores the
    // error (observed: 838:59:59.000).
    if (read.clamped && read.moment) return {Result::invalid, {}, temporal::text(*read.moment, kind, fsp)};
    if (!read.moment || read.truncated) return invalid();
    m = read.moment;
  } else {
    m = temporal::from_number(number_text(value), kind, kind == temporal::Kind::date ? 6 : fsp);
    if (!m) return invalid();
  }
  const bool lost = kind == temporal::Kind::date && (m->hour || m->minute || m->second || m->micro);
  temporal::round(*m, kind, fsp);
  if (type.kind == TypeKind::timestamp) {
    const auto at = temporal::ordinal(*m, temporal::Kind::datetime);
    const temporal::Moment first{false, 1970, 1, 1, 0, 0, 1, 0};
    const temporal::Moment last{false, 2038, 1, 19, 3, 14, 7, 999999};
    if (at < temporal::ordinal(first, temporal::Kind::datetime) || at > temporal::ordinal(last, temporal::Kind::datetime)) return invalid();
  }
  return {lost ? Result::truncated : Result::stored, temporal::text(*m, kind, fsp), {}};
}

// A value into a YEAR column: 1901 to 2155, or 0; a number 1 to 69 the
// 2000s and 70 to 99 the 1900s, and a string of one or two digits so too
// ('0' and '00' are 2000); past that out of range (1264).
Stored store_year(const Value& value, const ResultType& from) {
  int64_t year = 0;
  bool short_string = false;
  const auto* s = std::get_if<std::string>(&value);
  if (s && !from.number_string) {
    const auto t = trim(*s);
    if (t.empty() || t.size() > 6 || !std::all_of(t.begin(), t.end(), is_digit)) return {Result::invalid, {}, "0"};
    year = std::stoll(std::string(t));
    short_string = t.size() <= 2;
  } else if (const auto* d = std::get_if<Decimal>(&value)) {
    year = d->rounded(0).to_int().value_or(-1);
  } else if (const auto* r = std::get_if<double>(&value)) {
    year = std::llround(std::clamp(*r, -1.0, 100000.0));
  } else {
    const auto text = number_text(value);
    year = text.size() > 6 ? -1 : std::stoll(text);
  }
  if (short_string || (year >= 1 && year <= 99)) year = year < 70 ? 2000 + year : 1900 + year;
  if (year != 0 && (year < 1901 || year > 2155)) return {Result::out_of_range, {}, "0"};
  return {Result::stored, std::to_string(year), {}};
}

}  // namespace

size_t character_end(const std::string& s, uint64_t count) {
  size_t i = 0;
  for (uint64_t n = 0; n < count && i < s.size(); ++n) {
    ++i;
    while (i < s.size() && (static_cast<uint8_t>(s[i]) & 0xc0) == 0x80) ++i;
  }
  return i;
}

Stored store(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
  if (catalog::is_integer(type.kind)) return store_integer(type, value, from);
  if (type.kind == TypeKind::decimal) return store_decimal(type, value, from);
  if (type.kind == TypeKind::float_ || type.kind == TypeKind::double_) return store_real(type, value, from);
  if (catalog::is_temporal(type.kind)) return store_temporal(type, value, from);
  if (type.kind == TypeKind::year) return store_year(value, from);
  return store_string(type, value);
}

std::string implicit_default(const catalog::ColumnType& type) {
  if (catalog::is_integer(type.kind) || type.kind == TypeKind::float_ || type.kind == TypeKind::double_ || type.kind == TypeKind::year) return "0";
  if (catalog::is_temporal(type.kind)) return zero_temporal(type);
  if (type.kind == TypeKind::decimal) return zero_text(type.scale);
  if (type.kind == TypeKind::binary) return std::string(type.length, '\0');
  return {};
}

}  // namespace kvasir::exec
