//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/store.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>

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
  Decimal x;
  bool lost = false;
  if (const auto p = as_exact(v, from, x, lost); p != Parse::ok) return {failure(p), {}};
  const auto r = x.rounded(0);
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
  if (compare(r, Decimal::from_int(least)) < 0 || compare(r, Decimal::from_uint(most)) > 0) return {Result::out_of_range, {}};
  return {Result::stored, r.to_string()};
}

Stored store_decimal(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
  Decimal x;
  bool lost = false;
  // A DECIMAL column calls any string it cannot read invalid (observed).
  if (const auto p = as_exact(value, from, x, lost); p != Parse::ok) return {p == Parse::too_big ? Result::out_of_range : Result::invalid, {}};
  const auto r = x.rounded(type.scale);
  if (type.is_unsigned && r.negative()) return {Result::out_of_range, {}};
  if (whole_digits(r) > static_cast<size_t>(type.length - type.scale)) return {Result::out_of_range, {}};
  return {lost || compare(x, r) != 0 ? Result::truncated : Result::stored, r.to_string()};
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

Stored store_real(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
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
      if (!n) return {Result::out_of_range, {}};
      d = static_cast<double>(*n);
    } else {
      const std::string text(trim(*s));
      // A DOUBLE or FLOAT column calls any string it cannot read truncated
      // (observed).
      if (!numeric(text)) return {Result::garbage, {}};
      d = std::strtod(text.c_str(), nullptr);
    }
  }
  if (!std::isfinite(d)) return {Result::out_of_range, {}};
  if (type.length > 0) {
    // FLOAT(M,D) and DOUBLE(M,D): rounded to D, and below 10^(M-D).
    const double scale = std::pow(10.0, type.scale);
    d = std::round(d * scale) / scale;
    if (std::fabs(d) >= std::pow(10.0, static_cast<int>(type.length) - type.scale)) return {Result::out_of_range, {}};
  }
  if (type.kind == TypeKind::float_ && std::fabs(d) > FLT_MAX) return {Result::out_of_range, {}};
  if (type.is_unsigned && d < 0) return {Result::out_of_range, {}};
  return {Result::stored, type.kind == TypeKind::float_ ? format_float(static_cast<float>(d)) : format_double(d)};
}

// Well-formed UTF-8, with no character past the BMP for utf8mb3.
bool valid_utf8(const std::string& s, bool three_bytes_at_most) {
  size_t i = 0;
  while (i < s.size()) {
    const auto b = static_cast<uint8_t>(s[i]);
    size_t length = 0;
    uint32_t cp = 0;
    if (b < 0x80) {
      ++i;
      continue;
    }
    if ((b & 0xe0) == 0xc0) {
      length = 2;
      cp = b & 0x1f;
    } else if ((b & 0xf0) == 0xe0) {
      length = 3;
      cp = b & 0x0f;
    } else if ((b & 0xf8) == 0xf0) {
      length = 4;
      cp = b & 0x07;
    } else {
      return false;
    }
    if (i + length > s.size()) return false;
    for (size_t k = 1; k < length; ++k) {
      const auto c = static_cast<uint8_t>(s[i + k]);
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3f);
    }
    if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000)) return false;
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    if (length == 4 && three_bytes_at_most) return false;
    i += length;
  }
  return true;
}

// Where the first `count` characters of UTF-8 text end.
size_t character_end(const std::string& s, uint64_t count) {
  size_t i = 0;
  for (uint64_t n = 0; n < count && i < s.size(); ++n) {
    ++i;
    while (i < s.size() && (static_cast<uint8_t>(s[i]) & 0xc0) == 0x80) ++i;
  }
  return i;
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
  if (characters && !valid_utf8(text, type.charset == "utf8mb3")) return {Result::invalid, {}};
  const bool fixed = type.kind == TypeKind::char_ || type.kind == TypeKind::varchar || type.kind == TypeKind::binary || type.kind == TypeKind::varbinary;
  const uint64_t limit = fixed ? type.length : catalog::blob_max_bytes(type.kind);
  // CHAR and VARCHAR count characters; the rest count bytes.
  const size_t end = characters && fixed ? character_end(text, limit) : std::min<size_t>(text.size(), limit);
  Result result = Result::stored;
  if (end < text.size()) {
    if (text.find_first_not_of(' ', end) != std::string::npos) return {Result::too_long, {}};
    text.resize(end);
    if (type.kind != TypeKind::char_) result = Result::truncated;
  }
  if (type.kind == TypeKind::char_) {
    // Sent without its trailing spaces.
    while (!text.empty() && text.back() == ' ') text.pop_back();
  } else if (type.kind == TypeKind::binary) {
    text.resize(type.length, '\0');
  }
  return {result, text};
}

}  // namespace

Stored store(const catalog::ColumnType& type, const Value& value, const ResultType& from) {
  if (catalog::is_integer(type.kind)) return store_integer(type, value, from);
  if (type.kind == TypeKind::decimal) return store_decimal(type, value, from);
  if (type.kind == TypeKind::float_ || type.kind == TypeKind::double_) return store_real(type, value, from);
  return store_string(type, value);
}

}  // namespace kvasir::exec
