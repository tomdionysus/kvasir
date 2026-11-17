//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The string functions, typed and evaluated as MySQL 8.4.13 does (the
// manual's "String Functions and Operators", as observed).

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "kvasir/exec/expression.h"
#include "kvasir/protocol/types.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;
using sql::Expr;

constexpr uint16_t binary_collation = protocol::collation::binary;
// The longest a VARCHAR result can be, in bytes; longer is a BLOB.
constexpr uint64_t max_varchar_bytes = 65535;
constexpr uint64_t max_medium_blob_bytes = 16777215;
// max_allowed_packet's default: no string result can be longer.
constexpr uint64_t max_allowed_packet = 67108864;
// The length of a DOUBLE's text, as a string result counts it (observed).
constexpr uint64_t double_text_length = 22;

[[noreturn]] void wrong_count(const std::string& name) {
  throw SqlError(1582, "42000", "Incorrect parameter count in the call to native function '" + name + "'");
}

size_t characters(std::string_view s) {
  size_t n = 0;
  for (const char ch : s)
    if ((static_cast<uint8_t>(ch) & 0xc0) != 0x80) ++n;
  return n;
}

bool is_length(const std::string& name) { return name == "LENGTH" || name == "OCTET_LENGTH" || name == "CHAR_LENGTH" || name == "CHARACTER_LENGTH"; }
bool is_substring(const std::string& name) { return name == "SUBSTRING" || name == "SUBSTR" || name == "MID"; }
bool is_left_right(const std::string& name) { return name == "LEFT" || name == "RIGHT"; }

// Whether an expression is a constant, whose value MySQL reads when typing
// it: literals and operators over them.
bool is_constant(const Expr& e) {
  switch (e.kind) {
    case sql::ExprKind::null:
    case sql::ExprKind::boolean:
    case sql::ExprKind::integer:
    case sql::ExprKind::decimal:
    case sql::ExprKind::floating:
    case sql::ExprKind::string:
    case sql::ExprKind::hex:
    case sql::ExprKind::bits:
      return true;
    case sql::ExprKind::unary:
    case sql::ExprKind::binary:
      return std::all_of(e.args.begin(), e.args.end(), [](const sql::ExprPtr& a) { return is_constant(*a); });
    default:
      return false;
  }
}

}  // namespace

std::optional<int64_t> Evaluator::integer_argument(const Expr& x) {
  constexpr int64_t most = std::numeric_limits<int64_t>::max(), least = std::numeric_limits<int64_t>::min();
  const auto t = type(x);
  const auto v = value(x);
  if (is_null(v)) return std::nullopt;
  if (const auto* i = std::get_if<int64_t>(&v)) return *i;
  if (const auto* u = std::get_if<uint64_t>(&v)) return *u > static_cast<uint64_t>(most) ? most : static_cast<int64_t>(*u);
  if (const auto* d = std::get_if<Decimal>(&v)) {
    const auto whole = d->rounded(0).to_int();
    return whole ? *whole : d->negative() ? least : most;
  }
  if (const auto* r = std::get_if<double>(&v)) {
    const double whole = std::nearbyint(*r);
    if (!(whole > -9223372036854775808.0)) return least;
    if (!(whole < 9223372036854775808.0)) return most;
    return static_cast<int64_t>(whole);
  }
  const auto& s = std::get<std::string>(v);
  if (t.number_string) {
    uint64_t out = 0;
    for (const char ch : s) out = (out << 8) | static_cast<uint8_t>(ch);
    return out > static_cast<uint64_t>(most) ? most : static_cast<int64_t>(out);
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  const bool negative = i < s.size() && s[i] == '-';
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
  uint64_t magnitude = 0;
  bool overflow = false;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    const auto digit = static_cast<uint64_t>(s[i] - '0');
    if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10) overflow = true;
    magnitude = overflow ? magnitude : magnitude * 10 + digit;
    ++i;
  }
  size_t rest = i;
  while (rest < s.size() && s[rest] == ' ') ++rest;
  if (rest != s.size()) _warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect INTEGER value: '" + s + "'"});
  if (negative) return overflow || magnitude > static_cast<uint64_t>(most) ? least : -static_cast<int64_t>(magnitude);
  return overflow || magnitude > static_cast<uint64_t>(most) ? most : static_cast<int64_t>(magnitude);
}

std::optional<Evaluator::Merged> Evaluator::string_function(const Expr& e) const {
  if (e.kind != sql::ExprKind::call) return std::nullopt;
  const auto& name = e.value;
  const bool concat = name == "CONCAT", concat_ws = name == "CONCAT_WS", length = is_length(name);
  const bool substring = is_substring(name), left_right = is_left_right(name);
  if (!concat && !concat_ws && !length && !substring && !left_right) return std::nullopt;
  if ((concat && e.args.empty()) || (concat_ws && e.args.size() < 2) || (length && e.args.size() != 1) ||
      (substring && (e.args.size() < 2 || e.args.size() > 3)) || (left_right && e.args.size() != 2))
    wrong_count(name);
  Merged m;
  if (length) {
    m.type = integer_type(9, false, !type(*e.args[0]).nullable());
    m.derivation = {binary_collation, numeric_coercibility};
    return m;
  }
  // The strings' collations aggregated: a number's is the connection's at
  // NUMERIC coercibility, NULL's binary at IGNORABLE.
  const size_t strings = concat || concat_ws ? e.args.size() : 1;
  std::vector<ResultType> types;
  std::vector<Derivation> collations;
  for (size_t i = 0; i < strings; ++i) {
    const auto& a = e.args[i];
    types.push_back(type(*a));
    const auto& t = types.back();
    if (t.field_type == f::null) {
      collations.push_back({binary_collation, ignorable_coercibility});
    } else if (is_string_type(t)) {
      collations.push_back(derivation(*a, t));
    } else {
      collations.push_back({_settings.result_collation, numeric_coercibility});
    }
  }
  m.derivation = aggregate_collations(collations, concat ? "concat" : concat_ws ? "concat_ws" : "substr");
  const bool binary = m.derivation.first == binary_collation;
  // Each argument's text at its longest: characters, or bytes when the
  // result is binary.
  const auto longest = [&](const ResultType& t) -> uint64_t {
    if (t.field_type == f::null) return 0;
    if (is_string_type(t)) return binary || t.collation == binary_collation ? t.length : t.length / std::max<uint32_t>(_settings.result_max_bytes, 1);
    if ((t.field_type == f::double_ || t.field_type == f::float_) && t.decimals == protocol::not_fixed_decimals) return double_text_length;
    return t.length;
  };
  uint64_t total = 0;
  if (concat || concat_ws) {
    for (size_t i = concat_ws ? 1 : 0; i < types.size(); ++i) total += longest(types[i]);
    // CONCAT_WS: a separator between each two arguments, NULL or not.
    if (concat_ws) total += longest(types[0]) * (types.size() - 2);
  } else {
    // A piece of the string: what a constant position or length takes
    // away (nothing more once one is NULL).
    total = longest(types[0]);
    const auto constant = [&](size_t i) -> std::optional<std::optional<int64_t>> {
      if (!is_constant(*e.args[i])) return std::nullopt;
      Evaluator scratch(_settings);
      return scratch.integer_argument(*e.args[i]);
    };
    const auto at_most = [&](int64_t n) { total = n <= 0 ? 0 : std::min(total, static_cast<uint64_t>(n)); };
    [&]() {
      if (left_right) {
        if (const auto n = constant(1); n && *n) at_most(**n);
        return;
      }
      if (const auto start = constant(1)) {
        if (!*start) return;
        const int64_t s = **start;
        const uint64_t from_end = 0 - static_cast<uint64_t>(s);
        if (s < 0) {
          total = from_end > total ? 0 : from_end;
        } else {
          total -= std::min(static_cast<uint64_t>(s) - 1, total);
        }
      }
      if (e.args.size() == 3)
        if (const auto n = constant(2); n && *n) at_most(**n);
    }();
  }
  const uint64_t bytes = binary ? total : total * _settings.result_max_bytes;
  ResultType& t = m.type;
  t.collation = binary ? binary_collation : _settings.result_collation;
  t.decimals = protocol::not_fixed_decimals;
  t.flags = static_cast<uint16_t>(binary || is_bin_collation(m.derivation.first) ? c::binary : 0);
  if (bytes > max_varchar_bytes) {
    // A BLOB, whose text length is counted in characters once more
    // (observed), and at most max_allowed_packet.
    t.field_type = bytes > max_medium_blob_bytes ? f::long_blob : f::medium_blob;
    t.length = static_cast<uint32_t>(std::min(binary ? bytes : bytes * _settings.result_max_bytes, max_allowed_packet));
  } else {
    t.field_type = f::var_string;
    t.length = static_cast<uint32_t>(bytes);
  }
  return m;
}

Value Evaluator::string_function_value(const Expr& e) {
  const auto& name = e.value;
  // An argument's text, or nothing for NULL.
  const auto text = [&](const Expr& a) -> std::optional<std::string> { return to_text(value(a), type(a)); };
  if (is_length(name)) {
    const auto t = type(*e.args[0]);
    const auto s = text(*e.args[0]);
    if (!s) return {};
    // A binary string's characters are its bytes.
    if (name == "LENGTH" || name == "OCTET_LENGTH" || (is_string_type(t) && t.collation == binary_collation)) return static_cast<int64_t>(s->size());
    return static_cast<int64_t>(characters(*s));
  }
  if (is_substring(name) || is_left_right(name)) {
    const bool bytes = string_function(e)->type.collation == binary_collation;
    const auto s = text(*e.args[0]);
    if (!s) return {};
    const auto n = integer_argument(*e.args[1]);
    if (!n) return {};
    std::optional<int64_t> length;
    if (e.args.size() == 3) {
      length = integer_argument(*e.args[2]);
      if (!length) return {};
    }
    // Where each character starts, and the end.
    std::vector<size_t> starts;
    for (size_t i = 0; i < s->size(); ++i)
      if (bytes || (static_cast<uint8_t>((*s)[i]) & 0xc0) != 0x80) starts.push_back(i);
    const auto count = static_cast<int64_t>(starts.size());
    starts.push_back(s->size());
    int64_t begin = 0, end = count;
    if (name == "LEFT") {
      end = *n <= 0 ? 0 : std::min(*n, count);
    } else if (name == "RIGHT") {
      begin = *n <= 0 ? count : count - std::min(*n, count);
    } else {
      // From 1, or from the end when negative; 0 or past either end is
      // nothing.
      if (*n == 0 || *n > count || *n < -count) return std::string();
      begin = *n > 0 ? *n - 1 : count + *n;
      if (length) end = *length <= 0 ? begin : *length >= count - begin ? count : begin + *length;
    }
    const auto from = starts[static_cast<size_t>(begin)];
    return s->substr(from, starts[static_cast<size_t>(end)] - from);
  }
  std::string out;
  if (name == "CONCAT") {
    for (const auto& a : e.args) {
      const auto s = text(*a);
      if (!s) return {};
      out += *s;
    }
    return out;
  }
  // CONCAT_WS: NULL for a NULL separator; NULL arguments are skipped.
  const auto separator = text(*e.args[0]);
  if (!separator) return {};
  bool first = true;
  for (size_t i = 1; i < e.args.size(); ++i) {
    const auto s = text(*e.args[i]);
    if (!s) continue;
    if (!first) out += *separator;
    out += *s;
    first = false;
  }
  return out;
}

}  // namespace kvasir::exec
