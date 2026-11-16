//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/expression.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "kvasir/exec/collation.h"
#include "kvasir/exec/variables.h"
#include "kvasir/protocol/types.h"
#include "kvasir/sql/keywords.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;
using sql::Expr;
using sql::ExprKind;
// Wide enough that no sum or product of two 64-bit operands overflows it
// unseen; GCC and Clang both have it.
__extension__ typedef __int128 Int;

constexpr uint16_t binary_collation = protocol::collation::binary;
constexpr uint8_t any_decimals = protocol::not_fixed_decimals;
// The length MySQL gives a computed DOUBLE.
constexpr uint32_t double_length = 23;

[[noreturn]] void not_yet(const std::string& what) {
  throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support '" + what + "'");
}

ResultType integer_type(int precision, bool is_unsigned, bool not_null) {
  ResultType t;
  t.field_type = f::longlong;
  t.collation = binary_collation;
  t.precision = precision;
  t.length = static_cast<uint32_t>(precision + (is_unsigned ? 0 : 1));
  t.flags = static_cast<uint16_t>(c::binary | c::num | (is_unsigned ? c::unsigned_ : 0) | (not_null ? c::not_null : 0));
  return t;
}

ResultType boolean_type(bool not_null) {
  auto t = integer_type(1, false, not_null);
  t.length = 1;
  return t;
}

ResultType decimal_type(int precision, int decimals, bool not_null) {
  ResultType t;
  t.field_type = f::newdecimal;
  t.collation = binary_collation;
  t.precision = precision;
  t.decimals = static_cast<uint8_t>(decimals);
  t.length = static_cast<uint32_t>(precision + (decimals > 0 ? 1 : 0) + 1);
  t.flags = static_cast<uint16_t>(c::binary | c::num | (not_null ? c::not_null : 0));
  return t;
}

ResultType double_type(uint32_t length, bool not_null) {
  ResultType t;
  t.field_type = f::double_;
  t.collation = binary_collation;
  t.length = length;
  t.decimals = any_decimals;
  t.precision = static_cast<int>(length) - 1;
  t.flags = static_cast<uint16_t>(c::binary | c::num | (not_null ? c::not_null : 0));
  return t;
}

bool not_null(const ResultType& t) { return (t.flags & c::not_null) != 0; }

// How arithmetic treats an operand.
enum class Domain { integer, decimal, real };

Domain domain_of(const ResultType& t) {
  if (t.field_type == f::longlong) return Domain::integer;
  if (t.field_type == f::newdecimal) return Domain::decimal;
  // A hex or bit literal is an unsigned integer in a numeric context.
  if (t.number_string) return Domain::integer;
  return Domain::real;
}

bool is_string(const ResultType& t) {
  return t.field_type == f::var_string || t.field_type == f::blob || t.field_type == f::medium_blob || t.field_type == f::long_blob;
}
bool is_text(const ResultType& t) { return is_string(t) && t.collation != binary_collation; }

size_t characters(const std::string& s) {
  size_t n = 0;
  for (const char ch : s)
    if ((static_cast<uint8_t>(ch) & 0xc0) != 0x80) ++n;
  return n;
}

bool arithmetic(const std::string& op) { return op == "+" || op == "-" || op == "*" || op == "/" || op == "DIV" || op == "%"; }
bool comparison(const std::string& op) { return op == "=" || op == "<=>" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">="; }
bool logical(const std::string& op) { return op == "AND" || op == "OR" || op == "XOR"; }
bool bitwise(const std::string& op) { return op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>"; }

// The written digits of an integer literal, without leading zeros.
std::string_view significant(std::string_view digits) {
  const auto first = digits.find_first_not_of('0');
  return first == std::string_view::npos ? std::string_view("0") : digits.substr(first);
}

std::optional<uint64_t> parse_uint(std::string_view digits) {
  uint64_t v = 0;
  const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v);
  if (r.ec != std::errc() || r.ptr != digits.data() + digits.size()) return std::nullopt;
  return v;
}

double parse_double(std::string_view text) {
  double v = 0;
  std::from_chars(text.data(), text.data() + text.size(), v);
  return v;
}

int written_digits(std::string_view text) {
  int n = 0;
  for (const char ch : text)
    if (ch >= '0' && ch <= '9') ++n;
  return n;
}

}  // namespace

bool ResultType::nullable() const noexcept { return (flags & c::not_null) == 0; }
bool ResultType::is_unsigned() const noexcept { return (flags & c::unsigned_) != 0; }

std::optional<std::string> to_text(const Value& value, const ResultType& type) {
  struct Visitor {
    const ResultType& type;
    std::optional<std::string> operator()(std::monostate) const { return std::nullopt; }
    std::optional<std::string> operator()(int64_t v) const { return std::to_string(v); }
    std::optional<std::string> operator()(uint64_t v) const { return std::to_string(v); }
    std::optional<std::string> operator()(const Decimal& v) const {
      return type.own_scale || type.decimals == any_decimals ? v.to_string() : v.rounded(type.decimals).to_string();
    }
    std::optional<std::string> operator()(double v) const { return format_double(v); }
    std::optional<std::string> operator()(const std::string& v) const { return v; }
  };
  return std::visit(Visitor{type}, value);
}

Evaluator::Evaluator(EvalSettings settings) : _settings(settings) {}

ResultType Evaluator::type(const Expr& e) const {
  switch (e.kind) {
    case ExprKind::null: {
      ResultType t;
      t.field_type = f::null;
      t.collation = binary_collation;
      t.flags = c::binary | c::num;
      return t;
    }
    case ExprKind::boolean:
      return boolean_type(true);
    case ExprKind::integer: {
      const auto digits = significant(e.value);
      const int precision = static_cast<int>(e.value.size());
      const auto v = parse_uint(digits);
      if (v && *v <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return integer_type(precision, false, true);
      if (v) return integer_type(precision, true, true);
      return decimal_type(precision, 0, true);
    }
    case ExprKind::decimal: {
      const auto point = e.value.find('.');
      return decimal_type(written_digits(e.value), static_cast<int>(e.value.size() - point - 1), true);
    }
    case ExprKind::floating:
      return double_type(static_cast<uint32_t>(e.value.size()), true);
    case ExprKind::string: {
      ResultType t;
      t.field_type = f::var_string;
      t.collation = _settings.result_collation;
      t.length = static_cast<uint32_t>(characters(e.value)) * _settings.result_max_bytes;
      t.decimals = any_decimals;
      t.flags = static_cast<uint16_t>(c::not_null | (_settings.result_collation_binary ? c::binary : 0));
      return t;
    }
    case ExprKind::hex:
    case ExprKind::bits: {
      ResultType t;
      t.field_type = f::var_string;
      t.collation = binary_collation;
      t.length = static_cast<uint32_t>(e.value.size());
      t.precision = static_cast<int>(e.value.size());
      t.number_string = true;
      t.flags = static_cast<uint16_t>(c::not_null | c::binary | (e.kind == ExprKind::hex ? c::unsigned_ : 0));
      return t;
    }
    case ExprKind::unary: {
      const auto a = type(*e.args[0]);
      if (e.value == "NOT") return boolean_type(not_null(a));
      if (e.value == "~") {
        auto t = integer_type(21, true, not_null(a));
        return t;
      }
      if (e.value != "-") not_yet(e.value);
      const auto domain = domain_of(a);
      if (a.field_type == f::null || is_text(a) || domain == Domain::real) return double_type(double_length, not_null(a));
      if (domain == Domain::decimal) return decimal_type(a.precision, a.decimals, not_null(a));
      // A constant at or past 2^63 as an unsigned word becomes DECIMAL, but for
      // the literal 9223372036854775808.
      Evaluator scratch(_settings);
      const auto v = scratch.value(*e.args[0]);
      uint64_t bits = 0;
      if (const auto* i = std::get_if<int64_t>(&v)) bits = static_cast<uint64_t>(*i);
      if (const auto* u = std::get_if<uint64_t>(&v)) bits = *u;
      const uint64_t least = uint64_t{1} << 63;
      if (bits >= least && !(bits == least && e.args[0]->kind == ExprKind::integer)) return decimal_type(a.precision, 0, not_null(a));
      return integer_type(a.precision, false, not_null(a));
    }
    case ExprKind::binary: {
      const auto a = type(*e.args[0]);
      const auto b = type(*e.args[1]);
      const auto& op = e.value;
      const bool both = not_null(a) && not_null(b);
      if (comparison(op)) {
        string_collation(*e.args[0], a, *e.args[1], b);
        return boolean_type(op == "<=>" || both);
      }
      if (logical(op)) return boolean_type(both);
      if (bitwise(op)) return integer_type(21, true, both);
      if (!arithmetic(op)) not_yet(op);
      const auto da = domain_of(a);
      const auto db = domain_of(b);
      const bool real = da == Domain::real || db == Domain::real;
      const bool decimal = !real && (da == Domain::decimal || db == Domain::decimal);
      const bool divides = op == "/" || op == "DIV" || op == "%";
      const bool result_not_null = both && !divides;
      const int pa = a.precision, pb = b.precision;
      const int sa = da == Domain::decimal ? a.decimals : 0, sb = db == Domain::decimal ? b.decimals : 0;
      if (op == "DIV") {
        if (real) return integer_type(21, false, false);
        return integer_type(pa - sa + sb, a.is_unsigned() || b.is_unsigned(), false);
      }
      if (real) return double_type(double_length, result_not_null);
      if (op == "/") {
        const int scale = std::min(sa + _settings.div_precision_increment, static_cast<int>(decimal_max_scale));
        return decimal_type(pa - sa + sb + scale, scale, false);
      }
      if (decimal) {
        // A product's precision stops at 65; a sum's does not (observed).
        if (op == "*")
          return decimal_type(std::min(pa + pb, static_cast<int>(decimal_max_precision)), std::min(sa + sb, static_cast<int>(decimal_max_scale)),
                              result_not_null);
        const int scale = std::max(sa, sb);
        const int whole = std::max(pa - sa, pb - sb);
        if (op == "%") return decimal_type(whole + scale, scale, false);
        return decimal_type(whole + 1 + scale, scale, result_not_null);
      }
      if (op == "*") return integer_type(pa + pb, a.is_unsigned() || b.is_unsigned(), result_not_null);
      if (op == "%") return integer_type(std::max(pa, pb), a.is_unsigned(), false);
      return integer_type(std::max(pa, pb) + 1, a.is_unsigned() || b.is_unsigned(), result_not_null);
    }
    case ExprKind::is:
      type(*e.args[0]);
      return boolean_type(true);
    case ExprKind::in:
    case ExprKind::between: {
      bool all = true;
      const auto first = type(*e.args[0]);
      for (size_t i = 0; i < e.args.size(); ++i) {
        const auto t = i ? type(*e.args[i]) : first;
        if (i) string_collation(*e.args[0], first, *e.args[i], t);
        all = all && not_null(t);
      }
      return boolean_type(all);
    }
    case ExprKind::like: {
      const auto a = type(*e.args[0]);
      const auto b = type(*e.args[1]);
      string_collation(*e.args[0], a, *e.args[1], b);
      if (e.args.size() > 2) type(*e.args[2]);
      return boolean_type(not_null(a) && not_null(b));
    }
    case ExprKind::regexp:
      not_yet("REGEXP");
    case ExprKind::column: {
      std::string name;
      for (size_t i = 0; i < e.path.size(); ++i) name += (i ? "." : "") + e.path[i];
      throw SqlError(er_bad_field_error, "42S22", "Unknown column '" + name + "' in 'field list'");
    }
    case ExprKind::call:
      if (e.args.empty() && !e.star && _settings.variables) {
        ResultType t;
        Value unused;
        if (_settings.variables->session_function(e.value, t, unused)) return t;
      }
      not_yet("function " + e.value);
    case ExprKind::user_variable:
      if (!_settings.variables) not_yet("variables");
      return _settings.variables->user_type(e.value);
    case ExprKind::system_variable:
      if (!_settings.variables) not_yet("variables");
      return _settings.variables->system_type(e.scope, e.value);
    default:
      not_yet("this expression");
  }
}

namespace {

// How strongly an operand holds its collation: the lower wins (the manual's
// "Collation Coercibility in Expressions").
constexpr int implicit_coercibility = 2, sysconst_coercibility = 3, coercible_coercibility = 4;
// What system variables and session functions compare under (observed).
constexpr uint16_t utf8mb3_general_ci = 33;

struct Derivation {
  uint16_t collation;
  int coercibility;
};

bool is_bin(uint16_t collation) { return collation == 46 || collation == 309; }

}  // namespace

std::optional<uint16_t> Evaluator::string_collation(const Expr& x, const ResultType& tx, const Expr& y, const ResultType& ty) const {
  if (!is_string(tx) || !is_string(ty)) return std::nullopt;
  const auto derive = [&](const Expr& e, const ResultType& t) -> Derivation {
    switch (e.kind) {
      case ExprKind::string:
        if (e.charset.empty()) return {t.collation, coercible_coercibility};
        if (sql::same_word(e.charset, "binary")) return {binary_collation, coercible_coercibility};
        if (sql::same_word(e.charset, "utf8mb4")) return {protocol::collation::utf8mb4_0900_ai_ci, coercible_coercibility};
        not_yet("comparing strings in character set " + (e.charset == "national" ? std::string("utf8mb3") : e.charset));
      case ExprKind::hex:
      case ExprKind::bits:
        return {binary_collation, coercible_coercibility};
      case ExprKind::user_variable:
        return {t.collation, implicit_coercibility};
      case ExprKind::system_variable:
      case ExprKind::call:
        return {utf8mb3_general_ci, sysconst_coercibility};
      default:
        not_yet("comparing these strings");
    }
  };
  const auto a = derive(x, tx);
  const auto b = derive(y, ty);
  uint16_t chosen = a.collation;
  if (a.collation == binary_collation || b.collation == binary_collation) {
    chosen = binary_collation;
  } else if (a.coercibility != b.coercibility) {
    chosen = a.coercibility < b.coercibility ? a.collation : b.collation;
  } else if (a.collation != b.collation) {
    // One character set, a _bin collation beside another: the _bin one.
    if (is_bin(a.collation) == is_bin(b.collation)) not_yet("comparing strings of two collations");
    chosen = is_bin(a.collation) ? a.collation : b.collation;
  }
  if (!collation_by_id(chosen)) {
    std::string name = chosen == utf8mb3_general_ci ? "utf8mb3_general_ci" : std::to_string(chosen);
    for (const auto* c : {"utf8mb4_general_ci", "utf8mb4_unicode_ci"})
      if (const auto* info = find_collation(c); info && info->id == chosen) name = c;
    not_yet("comparing strings under " + name);
  }
  return chosen;
}

namespace {

// A numeric operand, read as one domain.
Int as_int(const Value& v) {
  if (const auto* i = std::get_if<int64_t>(&v)) return *i;
  if (const auto* u = std::get_if<uint64_t>(&v)) return *u;
  if (const auto* s = std::get_if<std::string>(&v)) {
    // A binary string is an unsigned integer, big-endian.
    uint64_t out = 0;
    for (const char ch : *s) out = (out << 8) | static_cast<uint8_t>(ch);
    return out;
  }
  return 0;
}

Decimal as_decimal(const Value& v) {
  if (const auto* d = std::get_if<Decimal>(&v)) return *d;
  if (const auto* i = std::get_if<int64_t>(&v)) return Decimal::from_int(*i);
  if (const auto* u = std::get_if<uint64_t>(&v)) return Decimal::from_uint(*u);
  return Decimal::from_uint(static_cast<uint64_t>(as_int(v)));
}

}  // namespace

Value Evaluator::value(const Expr& e) {
  // A string read as a number: its longest numeric prefix, with a warning
  // when anything but trailing space is left.
  const auto to_double = [&](const Value& v, const ResultType& t) -> double {
    if (const auto* d = std::get_if<double>(&v)) return *d;
    if (const auto* d = std::get_if<Decimal>(&v)) return d->to_double();
    if (const auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
    if (const auto* u = std::get_if<uint64_t>(&v)) return static_cast<double>(*u);
    const auto& s = std::get<std::string>(v);
    if (!is_text(t)) return static_cast<double>(static_cast<uint64_t>(as_int(v)));
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    const size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    size_t digits = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i, ++digits;
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i, ++digits;
    }
    size_t end = digits ? i : start;
    if (digits && i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      size_t k = i + 1;
      if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
      if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
        while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
        end = k;
      }
    }
    size_t rest = end;
    while (rest < s.size() && s[rest] == ' ') ++rest;
    if (rest != s.size()) _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect DOUBLE value: '" + s + "'"});
    std::string number(s.substr(start, end - start));
    if (!number.empty() && number[0] == '+') number.erase(0, 1);
    return number.empty() ? 0.0 : parse_double(number);
  };
  const auto truth = [&](const Value& v, const ResultType& t) -> std::optional<bool> {
    if (is_null(v)) return std::nullopt;
    if (const auto* d = std::get_if<Decimal>(&v)) return !d->is_zero();
    if (std::holds_alternative<int64_t>(v) || std::holds_alternative<uint64_t>(v)) return as_int(v) != 0;
    return to_double(v, t) != 0;
  };
  const auto boolean = [](std::optional<bool> b) -> Value {
    if (!b) return {};
    return int64_t{*b ? 1 : 0};
  };
  const auto division_by_zero = [&]() -> Value {
    _warnings.push_back({"Warning", er_division_by_zero, "Division by 0"});
    return {};
  };
  // -1, 0 or 1; nothing when either is NULL. Two strings compare under the
  // collation derived for the pair.
  const auto compare_values = [&](const Expr& ex, const Value& x, const ResultType& tx, const Expr& ey, const Value& y,
                                  const ResultType& ty) -> std::optional<int> {
    const auto collation = string_collation(ex, tx, ey, ty);
    if (is_null(x) || is_null(y)) return std::nullopt;
    if (collation) {
      const auto* sx = std::get_if<std::string>(&x);
      const auto* sy = std::get_if<std::string>(&y);
      if (sx && sy) return collation_by_id(*collation)->compare(*sx, *sy);
    }
    const auto dx = domain_of(tx), dy = domain_of(ty);
    if (dx == Domain::integer && dy == Domain::integer) {
      const Int a = as_int(x), b = as_int(y);
      return a < b ? -1 : a > b ? 1 : 0;
    }
    if (dx == Domain::real || dy == Domain::real) {
      const double a = to_double(x, tx), b = to_double(y, ty);
      return a < b ? -1 : a > b ? 1 : 0;
    }
    return compare(as_decimal(x), as_decimal(y));
  };
  const auto out_of_range = [&](const char* what) -> SqlError {
    return SqlError(er_data_out_of_range, "22003", std::string(what) + " value is out of range in '" + mysql_text(e) + "'");
  };

  switch (e.kind) {
    case ExprKind::null:
      return {};
    case ExprKind::boolean:
      return int64_t{e.value == "TRUE" ? 1 : 0};
    case ExprKind::integer: {
      const auto digits = significant(e.value);
      const auto v = parse_uint(digits);
      if (v && *v <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return static_cast<int64_t>(*v);
      if (v) return *v;
      return *Decimal::parse(digits);
    }
    case ExprKind::decimal:
      return *Decimal::parse(e.value);
    case ExprKind::floating:
      return parse_double(e.value);
    case ExprKind::string:
    case ExprKind::hex:
    case ExprKind::bits:
      return e.value;
    case ExprKind::unary: {
      const auto t = type(e);
      const auto at = type(*e.args[0]);
      const auto a = value(*e.args[0]);
      if (e.value == "NOT") {
        const auto b = truth(a, at);
        return boolean(b ? std::optional<bool>(!*b) : std::nullopt);
      }
      if (is_null(a)) return {};
      if (e.value == "~") {
        if (const auto* d = std::get_if<Decimal>(&a)) return ~static_cast<uint64_t>(d->rounded(0).to_int().value_or(0));
        if (const auto* r = std::get_if<double>(&a)) return ~static_cast<uint64_t>(std::llround(*r));
        return ~static_cast<uint64_t>(as_int(a));
      }
      // "-"
      if (t.field_type == f::double_) return -to_double(a, at);
      if (t.field_type == f::newdecimal) return as_decimal(a).negated();
      const Int negated = -as_int(a);
      if (negated < std::numeric_limits<int64_t>::min() || negated > std::numeric_limits<int64_t>::max()) throw out_of_range("BIGINT");
      return static_cast<int64_t>(negated);
    }
    case ExprKind::binary: {
      const auto& op = e.value;
      const auto ta = type(*e.args[0]);
      const auto tb = type(*e.args[1]);
      if (logical(op)) {
        const auto x = truth(value(*e.args[0]), ta);
        const auto y = truth(value(*e.args[1]), tb);
        if (op == "AND") {
          if ((x && !*x) || (y && !*y)) return int64_t{0};
          if (!x || !y) return {};
          return int64_t{1};
        }
        if (op == "OR") {
          if ((x && *x) || (y && *y)) return int64_t{1};
          if (!x || !y) return {};
          return int64_t{0};
        }
        if (!x || !y) return {};
        return int64_t{*x != *y ? 1 : 0};
      }
      const auto a = value(*e.args[0]);
      const auto b = value(*e.args[1]);
      if (comparison(op)) {
        if (op == "<=>") {
          if (is_null(a) || is_null(b)) return int64_t{is_null(a) && is_null(b) ? 1 : 0};
        }
        const auto c3 = compare_values(*e.args[0], a, ta, *e.args[1], b, tb);
        if (!c3) return {};
        const int r = *c3;
        bool result = false;
        if (op == "=" || op == "<=>") result = r == 0;
        if (op == "!=") result = r != 0;
        if (op == "<") result = r < 0;
        if (op == "<=") result = r <= 0;
        if (op == ">") result = r > 0;
        if (op == ">=") result = r >= 0;
        return int64_t{result ? 1 : 0};
      }
      if (is_null(a) || is_null(b)) return {};
      if (bitwise(op)) {
        const auto bits = [&](const Value& v) -> uint64_t {
          if (const auto* d = std::get_if<Decimal>(&v)) return static_cast<uint64_t>(d->rounded(0).to_int().value_or(0));
          if (const auto* r = std::get_if<double>(&v)) return static_cast<uint64_t>(std::llround(*r));
          return static_cast<uint64_t>(as_int(v));
        };
        const uint64_t x = bits(a), y = bits(b);
        if (op == "&") return x & y;
        if (op == "|") return x | y;
        if (op == "^") return x ^ y;
        if (op == "<<") return y >= 64 ? uint64_t{0} : x << y;
        return y >= 64 ? uint64_t{0} : x >> y;
      }
      const auto t = type(e);
      const auto da = domain_of(ta), db = domain_of(tb);
      if (op == "DIV") {
        if (da == Domain::real || db == Domain::real) {
          const double y = to_double(b, tb);
          if (y == 0) return division_by_zero();
          const double q = std::trunc(to_double(a, ta) / y);
          if (!(q >= -9223372036854775808.0 && q < 9223372036854775808.0)) throw out_of_range("BIGINT");
          return static_cast<int64_t>(q);
        }
        Int q = 0;
        if (da == Domain::decimal || db == Domain::decimal) {
          const auto exact = Decimal::divide(as_decimal(a), as_decimal(b), 0);
          if (!exact) return division_by_zero();
          const auto whole = exact->truncated(0);
          const auto fits = whole.to_int();
          if (!fits) throw out_of_range("BIGINT");
          q = *fits;
        } else {
          const Int y = as_int(b);
          if (y == 0) return division_by_zero();
          q = as_int(a) / y;
        }
        if (t.is_unsigned()) {
          if (q < 0 || q > std::numeric_limits<uint64_t>::max()) throw out_of_range("BIGINT UNSIGNED");
          return static_cast<uint64_t>(q);
        }
        if (q < std::numeric_limits<int64_t>::min() || q > std::numeric_limits<int64_t>::max()) throw out_of_range("BIGINT");
        return static_cast<int64_t>(q);
      }
      if (t.field_type == f::double_) {
        const double x = to_double(a, ta), y = to_double(b, tb);
        double r = 0;
        if (op == "+") r = x + y;
        if (op == "-") r = x - y;
        if (op == "*") r = x * y;
        if (op == "/" || op == "%") {
          if (y == 0) return division_by_zero();
          r = op == "/" ? x / y : std::fmod(x, y);
        }
        if (!std::isfinite(r)) throw out_of_range("DOUBLE");
        return r;
      }
      if (t.field_type == f::newdecimal) {
        const auto x = as_decimal(a), y = as_decimal(b);
        try {
          if (op == "+") return x + y;
          if (op == "-") return x - y;
          if (op == "*") return x * y;
          if (op == "/") {
            const auto q = Decimal::divide(x, y, _settings.div_precision_increment);
            if (!q) return division_by_zero();
            return *q;
          }
          const auto r = Decimal::remainder(x, y);
          if (!r) return division_by_zero();
          return *r;
        } catch (const std::overflow_error&) {
          throw out_of_range("DECIMAL");
        }
      }
      const Int x = as_int(a), y = as_int(b);
      Int r = 0;
      if (op == "+") r = x + y;
      if (op == "-") r = x - y;
      if (op == "*") {
        // Past 2^127 an __int128 product overflows; past 2^64 the result does anyway.
        const auto mag = [](Int v) { return v < 0 ? -v : v; };
        if (x != 0 && mag(y) > (Int(1) << 100) / mag(x)) throw out_of_range(t.is_unsigned() ? "BIGINT UNSIGNED" : "BIGINT");
        r = x * y;
      }
      if (op == "%") {
        if (y == 0) return division_by_zero();
        r = x % y;
      }
      if (t.is_unsigned()) {
        if (r < 0 || r > std::numeric_limits<uint64_t>::max()) throw out_of_range("BIGINT UNSIGNED");
        return static_cast<uint64_t>(r);
      }
      if (r < std::numeric_limits<int64_t>::min() || r > std::numeric_limits<int64_t>::max()) throw out_of_range("BIGINT");
      return static_cast<int64_t>(r);
    }
    case ExprKind::is: {
      const auto t = type(*e.args[0]);
      const auto v = value(*e.args[0]);
      bool result = false;
      if (e.value == "NULL" || e.value == "UNKNOWN") {
        result = is_null(v);
      } else {
        const auto b = truth(v, t);
        result = b && (*b == (e.value == "TRUE"));
      }
      return int64_t{(result != e.negated) ? 1 : 0};
    }
    case ExprKind::in: {
      const auto ta = type(*e.args[0]);
      const auto a = value(*e.args[0]);
      if (is_null(a)) return {};
      bool saw_null = false;
      for (size_t i = 1; i < e.args.size(); ++i) {
        const auto r = compare_values(*e.args[0], a, ta, *e.args[i], value(*e.args[i]), type(*e.args[i]));
        if (!r) {
          saw_null = true;
        } else if (*r == 0) {
          return int64_t{e.negated ? 0 : 1};
        }
      }
      if (saw_null) return {};
      return int64_t{e.negated ? 1 : 0};
    }
    case ExprKind::between: {
      const auto ta = type(*e.args[0]);
      const auto a = value(*e.args[0]);
      const auto low = compare_values(*e.args[0], a, ta, *e.args[1], value(*e.args[1]), type(*e.args[1]));
      const auto high = compare_values(*e.args[0], a, ta, *e.args[2], value(*e.args[2]), type(*e.args[2]));
      // a >= low AND a <= high, with NULL as unknown.
      const std::optional<bool> x = low ? std::optional<bool>(*low >= 0) : std::nullopt;
      const std::optional<bool> y = high ? std::optional<bool>(*high <= 0) : std::nullopt;
      std::optional<bool> r;
      if ((x && !*x) || (y && !*y))
        r = false;
      else if (x && y)
        r = true;
      if (r && e.negated) r = !*r;
      return boolean(r);
    }
    case ExprKind::like: {
      const auto ta = type(*e.args[0]);
      const auto tb = type(*e.args[1]);
      // The escape: one character, or none when empty; '\' when not given.
      std::string escape = "\\";
      if (e.args.size() > 2) {
        const auto text = to_text(value(*e.args[2]), type(*e.args[2]));
        if (!text || characters(*text) > 1) throw SqlError(er_wrong_arguments, "HY000", "Incorrect arguments to ESCAPE");
        escape = *text;
      }
      const auto a = value(*e.args[0]);
      const auto b = value(*e.args[1]);
      if (is_null(a) || is_null(b)) return {};
      // Two strings match under the pair's collation; a number by its text,
      // under the other side's.
      auto collation = string_collation(*e.args[0], ta, *e.args[1], tb);
      if (!collation) collation = is_string(ta) ? ta.collation : is_string(tb) ? tb.collation : _settings.result_collation;
      const auto* c = collation_by_id(*collation);
      if (!c) not_yet("LIKE under this collation");
      const bool matches = c->like(*to_text(a, ta), *to_text(b, tb), escape);
      return int64_t{matches != e.negated ? 1 : 0};
    }
    case ExprKind::call: {
      type(e);
      ResultType t;
      Value v;
      _settings.variables->session_function(e.value, t, v);
      return v;
    }
    case ExprKind::user_variable:
      if (!_settings.variables) not_yet("variables");
      return _settings.variables->user_value(e.value);
    case ExprKind::system_variable:
      if (!_settings.variables) not_yet("variables");
      return _settings.variables->system_value(e.scope, e.value);
    default:
      type(e);
      not_yet("this expression");
  }
}

std::string mysql_text(const Expr& e) {
  switch (e.kind) {
    case ExprKind::null:
      return "NULL";
    case ExprKind::boolean:
      return e.value == "TRUE" ? "true" : "false";
    case ExprKind::integer:
      return std::string(significant(e.value));
    case ExprKind::decimal:
    case ExprKind::floating:
      return e.value;
    case ExprKind::string:
      return "'" + e.value + "'";
    case ExprKind::unary:
      if (e.value == "-") return "-(" + mysql_text(*e.args[0]) + ")";
      if (e.value == "NOT") return "(not(" + mysql_text(*e.args[0]) + "))";
      return e.value + "(" + mysql_text(*e.args[0]) + ")";
    case ExprKind::binary: {
      std::string op = e.value;
      if (op == "!=") op = "<>";
      if (op == "AND") op = "and";
      if (op == "OR") op = "or";
      if (op == "XOR") op = "xor";
      return "(" + mysql_text(*e.args[0]) + " " + op + " " + mysql_text(*e.args[1]) + ")";
    }
    default:
      return sql::to_string(e);
  }
}

std::string column_name(const sql::SelectItem& item) {
  if (item.has_alias) return item.alias;
  if (item.star) return "*";
  // A generated name is held in utf8mb3, where a character past the BMP
  // (four bytes in UTF-8) becomes '?' (observed).
  const auto narrowed = [](const std::string& name) {
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
      if ((static_cast<uint8_t>(name[i]) & 0xf8) == 0xf0 && i + 3 < name.size()) {
        out.push_back('?');
        i += 3;
      } else {
        out.push_back(name[i]);
      }
    }
    return out;
  };
  if (item.expr && item.expr->kind == ExprKind::string) return narrowed(item.expr->path.empty() ? item.expr->value : item.expr->path.front());
  return narrowed(item.text);
}

}  // namespace kvasir::exec
