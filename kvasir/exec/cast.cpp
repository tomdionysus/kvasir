//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// CAST and CONVERT, typed and evaluated as MySQL 8.4.13 does (the manual's
// "Cast Functions and Operators", as observed).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>

#include "kvasir/exec/expression.h"
#include "kvasir/protocol/types.h"
#include "kvasir/sql/keywords.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;
using sql::Expr;

constexpr uint16_t binary_collation = protocol::collation::binary;
// utf8mb4's default collation, utf8mb4_0900_ai_ci.
constexpr uint16_t utf8mb4_collation = 255;
constexpr uint8_t any_decimals = protocol::not_fixed_decimals;
// DECIMAL's limits, and its type's when CAST names no precision.
constexpr uint64_t max_precision = 65;
constexpr uint64_t max_scale = 30;
constexpr uint64_t default_precision = 10;

[[noreturn]] void not_yet(const std::string& what) {
  throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support '" + what + "'");
}

// The length or scale written at args[i], or nothing.
std::optional<uint64_t> written(const Expr& e, size_t i) {
  if (e.args.size() <= i) return std::nullopt;
  uint64_t v = 0;
  const auto& text = e.args[i]->value;
  for (const char ch : text) {
    if (v > (std::numeric_limits<uint64_t>::max() - 9) / 10) return std::numeric_limits<uint64_t>::max();
    v = v * 10 + static_cast<uint64_t>(ch - '0');
  }
  return v;
}

bool continuation(char ch) { return (static_cast<uint8_t>(ch) & 0xC0) == 0x80; }

uint64_t utf8_characters(const std::string& s) {
  uint64_t n = 0;
  for (const char ch : s) n += continuation(ch) ? 0 : 1;
  return n;
}

// The bytes the first `n` characters of UTF-8 text take.
size_t utf8_prefix(const std::string& s, uint64_t n) {
  size_t at = 0;
  for (uint64_t k = 0; k < n && at < s.size(); ++k) {
    ++at;
    while (at < s.size() && continuation(s[at])) ++at;
  }
  return at;
}

// A string's leading number as CAST reads it: blanks, a sign, digits and
// (for a DECIMAL) a fraction; `rest` whether anything but blanks follows.
struct Leading {
  bool negative = false;
  std::string digits;
  std::string fraction;
  int exponent = 0;
  bool rest = false;

  // As a DECIMAL literal writes it: the exponent applied.
  std::string decimal() const {
    const auto all = digits + fraction;
    const auto point = static_cast<long>(digits.size()) + exponent;
    std::string out;
    if (all.empty()) {
      out = "0";
    } else if (point <= 0) {
      out = "0." + std::string(static_cast<size_t>(-point), '0') + all;
    } else if (static_cast<size_t>(point) >= all.size()) {
      out = all + std::string(static_cast<size_t>(point) - all.size(), '0');
    } else {
      out = all.substr(0, static_cast<size_t>(point)) + "." + all.substr(static_cast<size_t>(point));
    }
    return (negative ? "-" : "") + out;
  }
};

// A number's text read as far as it goes; with `fraction`, a point, digits
// and an exponent too (observed: '1.5e2' as DECIMAL is 150).
Leading leading(const std::string& s, bool fraction) {
  Leading out;
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) out.negative = s[i++] == '-';
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) out.digits.push_back(s[i++]);
  if (fraction && i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) out.fraction.push_back(s[i++]);
  }
  const bool any = !out.digits.empty() || !out.fraction.empty();
  if (fraction && any && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    size_t k = i + 1;
    const bool minus = k < s.size() && s[k] == '-';
    if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
    if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
      int e = 0;
      // Past 100 digits either way the value is out of any DECIMAL's range.
      while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) e = std::min(e * 10 + (s[k++] - '0'), 200);
      out.exponent = minus ? -e : e;
      i = k;
    }
  }
  while (i < s.size() && s[i] == ' ') ++i;
  out.rest = i != s.size() || !any;
  return out;
}

}  // namespace

ResultType Evaluator::cast_type(const Expr& e) const {
  const auto from = type(*e.args[0]);
  const bool nn = (from.flags & c::not_null) != 0;
  const auto& to = e.scope;
  if (to == "SIGNED" || to == "UNSIGNED") {
    // 21 long either way; UNSIGNED carries 21 digits into arithmetic,
    // SIGNED 20 (observed: CAST(1 AS UNSIGNED) + 1 is 22 long, CAST('7' AS
    // SIGNED) * 2 too).
    const bool to_unsigned = to == "UNSIGNED";
    auto t = integer_type(to_unsigned ? 21 : 20, to_unsigned, nn);
    t.length = 21;
    return t;
  }
  if (to == "DECIMAL") {
    const auto precision = written(e, 1).value_or(default_precision);
    const auto scale = written(e, 2).value_or(0);
    const auto name = mysql_text(*e.args[0], _settings.columns);
    if (precision > max_precision)
      throw SqlError(1426, "42000", "Too-big precision " + std::to_string(precision) + " specified for '" + name + "'. Maximum is 65.");
    if (scale > max_scale) throw SqlError(1425, "42000", "Too big scale " + std::to_string(scale) + " specified for column '" + name + "'. Maximum is 30.");
    if (scale > precision) throw SqlError(1427, "42000", "For float(M,D), double(M,D) or decimal(M,D), M must be >= D (column '').");
    return decimal_type(static_cast<int>(precision), static_cast<int>(scale), nn);
  }
  if (to == "DOUBLE" || to == "FLOAT") {
    auto t = double_type(23, nn);
    // FLOAT(p) is a DOUBLE past 24 bits of precision; past 53 it is refused,
    // naming the CAST (observed).
    const auto bits = written(e, 1).value_or(0);
    if (bits > 53) throw SqlError(1426, "42000", "Too-big precision " + std::to_string(bits) + " specified for 'CAST'. Maximum is 53.");
    if (to == "FLOAT" && bits <= 24) t.field_type = f::float_;
    return t;
  }
  if (to == "CHAR" || to == "BINARY") {
    const bool binary = to == "BINARY" || sql::same_word(e.charset, "binary");
    if (!binary && !e.charset.empty() && !sql::same_word(e.charset, "utf8mb4")) not_yet("CHARACTER SET " + e.charset);
    // As long as asked, or as the operand's text is: in characters, or as
    // BINARY in its bytes (observed: 'abc' as BINARY is 12 long).
    uint64_t length = 0;
    if (const auto n = written(e, 1)) {
      length = *n;
    } else if (binary && is_string_type(from)) {
      length = from.length;
    } else if (is_string_type(from) && from.collation != binary_collation) {
      length = from.length / widest_character(from.collation);
    } else if (from.field_type == f::double_ || from.field_type == f::float_) {
      // A DOUBLE's text, as a string result counts it (observed: 22).
      length = 22;
    } else {
      length = from.length;
    }
    ResultType t;
    t.field_type = f::var_string;
    t.decimals = any_decimals;
    if (binary) {
      t.collation = binary_collation;
      t.length = static_cast<uint32_t>(std::min<uint64_t>(length, std::numeric_limits<uint32_t>::max()));
      t.flags = c::binary;
      return t;
    }
    t.collation = e.charset.empty() ? _settings.result_collation : utf8mb4_collation;
    const uint64_t bytes = e.charset.empty() ? _settings.result_max_bytes : 4;
    t.length = static_cast<uint32_t>(std::min<uint64_t>(length * bytes, std::numeric_limits<uint32_t>::max()));
    if (e.charset.empty() && _settings.result_collation_binary) t.flags = c::binary;
    return t;
  }
  not_yet("CAST AS " + to);
}

Value Evaluator::cast_value(const Expr& e) {
  const auto t = cast_type(e);
  const auto from = type(*e.args[0]);
  const auto v = value(*e.args[0]);
  if (is_null(v)) return v;
  const auto& to = e.scope;
  const auto* text = std::get_if<std::string>(&v);
  const bool string = text && !from.number_string;
  if (to == "SIGNED" || to == "UNSIGNED") {
    const bool to_unsigned = to == "UNSIGNED";
    // A number rounds half away from zero; a string is read up to what is
    // not a digit (observed: '-2.5' is -2, warning 1292).
    if (string) {
      // Past 64 bits a positive number is 2^64 - 1 and a negative one -2^63,
      // with warning 1292; a positive number then keeps its 64 bits as
      // SIGNED, and a negative one as UNSIGNED warns 1105 (observed:
      // '99999999999999999999' is -1, '-1' is 2^64 - 1).
      const auto n = leading(*text, false);
      uint64_t magnitude = 0;
      bool over = false;
      for (const char ch : n.digits) {
        const auto digit = static_cast<uint64_t>(ch - '0');
        if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
          over = true;
          break;
        }
        magnitude = magnitude * 10 + digit;
      }
      constexpr uint64_t past_signed = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
      if (n.negative && magnitude > past_signed) over = true;
      if (n.rest || over) _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect INTEGER value: '" + *text + "'"});
      uint64_t bits = 0;
      if (n.negative) {
        bits = over ? past_signed : 0 - magnitude;
      } else {
        bits = over ? std::numeric_limits<uint64_t>::max() : magnitude;
      }
      if (to_unsigned) {
        if (n.negative && magnitude != 0) _warnings.push_back({"Warning", 1105, "Cast to unsigned converted negative integer to its positive complement"});
        return bits;
      }
      return static_cast<int64_t>(bits);
    }
    // Wrapped, as MySQL keeps the 64 bits (observed: 2^64 - 1 is -1 as
    // SIGNED, -1 is 2^64 - 1 as UNSIGNED).
    const auto bits = [&](const Value& n) -> uint64_t {
      if (const auto* i = std::get_if<int64_t>(&n)) return static_cast<uint64_t>(*i);
      if (const auto* u = std::get_if<uint64_t>(&n)) return *u;
      if (const auto* d = std::get_if<Decimal>(&n)) {
        const auto r = d->rounded(0);
        if (r.negative()) return static_cast<uint64_t>(r.to_int().value_or(std::numeric_limits<int64_t>::min()));
        return compare(r, Decimal::from_uint(std::numeric_limits<uint64_t>::max())) > 0 ? std::numeric_limits<uint64_t>::max() : std::stoull(r.to_string());
      }
      if (const auto* r = std::get_if<double>(&n)) {
        // A DOUBLE is clamped to the type's range (observed: 1e30 as SIGNED
        // is 2^63 - 1).
        const double x = std::round(*r);
        if (x <= -9223372036854775808.0) return static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
        if (x < 0) return static_cast<uint64_t>(static_cast<int64_t>(x));
        if (!to_unsigned && x >= 9223372036854775807.0) return static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (x >= 18446744073709551615.0) return std::numeric_limits<uint64_t>::max();
        return static_cast<uint64_t>(x);
      }
      // A hex or bit literal: its bytes, big-endian.
      uint64_t out = 0;
      for (const char ch : std::get<std::string>(n)) out = (out << 8) | static_cast<uint8_t>(ch);
      return out;
    };
    const auto b = bits(v);
    if (to_unsigned) return b;
    return static_cast<int64_t>(b);
  }
  if (to == "DECIMAL") {
    const auto precision = static_cast<int>(t.precision);
    const int scale = t.decimals;
    std::optional<Decimal> d;
    if (string) {
      const auto n = leading(*text, true);
      if (n.rest) _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect DECIMAL value: '" + *text + "'"});
      d = Decimal::parse(n.decimal());
    } else if (const auto* r = std::get_if<double>(&v)) {
      char buffer[400];
      std::snprintf(buffer, sizeof buffer, "%.*f", scale + 1, *r);
      d = Decimal::parse(buffer);
    } else if (const auto* x = std::get_if<Decimal>(&v)) {
      d = *x;
    } else if (const auto* i = std::get_if<int64_t>(&v)) {
      d = Decimal::from_int(*i);
    } else if (const auto* u = std::get_if<uint64_t>(&v)) {
      d = Decimal::from_uint(*u);
    } else {
      uint64_t out = 0;
      for (const char ch : std::get<std::string>(v)) out = (out << 8) | static_cast<uint8_t>(ch);
      d = Decimal::from_uint(out);
    }
    // Past the precision, the largest it holds, warning 1264 naming the
    // CAST as written (observed).
    const std::string nines = std::string(static_cast<size_t>(precision - scale), '9') + (scale ? "." + std::string(static_cast<size_t>(scale), '9') : "");
    const auto largest = *Decimal::parse(nines.empty() ? "0" : nines);
    const auto rounded = d ? d->rounded(scale) : largest;
    const bool negative = d && d->negative();
    const auto magnitude = negative ? rounded.negated() : rounded;
    if (!d || compare(magnitude, largest) > 0) {
      _warnings.push_back({"Warning", 1264, "Out of range value for column '" + e.path.at(0) + "' at row 1"});
      return negative ? largest.negated() : largest;
    }
    return rounded;
  }
  if (to == "DOUBLE" || to == "FLOAT") {
    const double r = real(v, from);
    if (t.field_type == f::float_) return static_cast<double>(static_cast<float>(r));
    return r;
  }
  // CHAR and BINARY: the operand's text, cut to the length asked with
  // warning 1292 (observed: "Truncated incorrect CHAR(2) value: '123'").
  std::string out = text ? *text : *to_text(v, from);
  if (const auto n = written(e, 1)) {
    const bool binary = t.collation == binary_collation;
    const size_t keep = binary ? std::min<uint64_t>(*n, out.size()) : utf8_prefix(out, *n);
    if (keep < out.size() || (!binary && utf8_characters(out) > *n)) {
      _warnings.push_back(
          {"Warning", er_truncated_wrong_value,
           "Truncated incorrect " + std::string(binary && to == "BINARY" ? "BINARY" : "CHAR") + "(" + std::to_string(*n) + ") value: '" + out + "'"});
      out.resize(keep);
    }
  }
  return out;
}

}  // namespace kvasir::exec
