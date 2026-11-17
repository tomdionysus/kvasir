//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The string functions, typed and evaluated as MySQL 8.4.13 does (the
// manual's "String Functions and Operators", as observed).

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "kvasir/exec/collation.h"
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
bool is_locate(const std::string& name) { return name == "LOCATE" || name == "INSTR" || name == "POSITION"; }
bool is_trim(const std::string& name) { return name == "TRIM" || name == "LTRIM" || name == "RTRIM"; }

// The warning for a result max_allowed_packet cannot hold, which is NULL.
Warning too_long(const std::string& name) {
  std::string lower;
  for (const char ch : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return {"Warning", 1301, "Result of " + lower + "() was larger than max_allowed_packet (67108864) - truncated"};
}

// Where each character of `s` starts, then its end: each byte when `bytes`.
std::vector<size_t> character_starts(const std::string& s, bool bytes) {
  std::vector<size_t> starts;
  for (size_t i = 0; i < s.size(); ++i)
    if (bytes || (static_cast<uint8_t>(s[i]) & 0xc0) != 0x80) starts.push_back(i);
  starts.push_back(s.size());
  return starts;
}

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

std::optional<std::optional<int64_t>> Evaluator::constant_integer(const Expr& x) const {
  if (!is_constant(x)) return std::nullopt;
  Evaluator scratch(_settings);
  return scratch.integer_argument(x);
}

std::optional<Evaluator::Merged> Evaluator::string_function(const Expr& e) const {
  if (e.kind != sql::ExprKind::call) return std::nullopt;
  const auto& name = e.value;
  // Each function and how many arguments it takes.
  struct Arity {
    std::string_view name;
    size_t least, most;
  };
  static constexpr size_t any = std::numeric_limits<size_t>::max();
  static constexpr std::array<Arity, 22> arities = {
      {{"CONCAT", 1, any},  {"CONCAT_WS", 2, any}, {"LENGTH", 1, 1},  {"OCTET_LENGTH", 1, 1}, {"CHAR_LENGTH", 1, 1}, {"CHARACTER_LENGTH", 1, 1},
       {"SUBSTRING", 2, 3}, {"SUBSTR", 2, 3},      {"MID", 2, 3},     {"LEFT", 2, 2},         {"RIGHT", 2, 2},       {"TRIM", 1, 2},
       {"LTRIM", 1, 1},     {"RTRIM", 1, 1},       {"REPLACE", 3, 3}, {"LOCATE", 2, 3},       {"INSTR", 2, 2},       {"POSITION", 2, 2},
       {"LPAD", 3, 3},      {"RPAD", 3, 3},        {"REPEAT", 2, 2},  {"REVERSE", 1, 1}}};
  const auto found = std::find_if(arities.begin(), arities.end(), [&](const Arity& a) { return a.name == name; });
  if (found == arities.end()) return std::nullopt;
  if (e.args.size() < found->least || e.args.size() > found->most) wrong_count(name);
  const bool concat = name == "CONCAT", concat_ws = name == "CONCAT_WS";
  const bool left_right = is_left_right(name);
  Merged m;
  if (is_length(name)) {
    m.type = integer_type(9, false, !type(*e.args[0]).nullable());
    m.derivation = {binary_collation, numeric_coercibility};
    return m;
  }
  if (is_locate(name)) {
    const bool not_null = std::all_of(e.args.begin(), e.args.end(), [&](const sql::ExprPtr& a) { return !type(*a).nullable(); });
    m.type = integer_type(10, false, not_null);
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
  // The others hold their first argument's collation.
  m.derivation = aggregate_collations(collations, concat ? "concat" : "concat_ws");
  const bool binary = m.derivation.first == binary_collation;
  // Each argument's text at its longest: characters, or bytes when the
  // result is binary.
  const auto longest = [&](const ResultType& t) -> uint64_t {
    if (t.field_type == f::null) return 0;
    if (is_string_type(t)) return binary || t.collation == binary_collation ? t.length : t.length / std::max<uint32_t>(_settings.result_max_bytes, 1);
    if ((t.field_type == f::double_ || t.field_type == f::float_) && t.decimals == protocol::not_fixed_decimals) return double_text_length;
    return t.length;
  };
  const auto constant = [&](size_t i) { return constant_integer(*e.args[i]); };
  // Past max_allowed_packet a length is as long as anything can be.
  constexpr uint64_t past_packet = max_allowed_packet + 1;
  uint64_t total = 0;
  // A length or count that is not a constant: as long as anything can be,
  // reported as max_allowed_packet in characters (observed).
  bool unbounded = false;
  if (concat || concat_ws) {
    for (size_t i = concat_ws ? 1 : 0; i < types.size(); ++i) total += longest(types[i]);
    // CONCAT_WS: a separator between each two arguments, NULL or not.
    if (concat_ws) total += longest(types[0]) * (types.size() - 2);
  } else if (is_trim(name) || name == "REVERSE") {
    total = longest(types[0]);
  } else if (name == "REPLACE") {
    // The string's length times one less than the replacement's, at least
    // once (observed).
    const uint64_t replacement = longest(type(*e.args[2]));
    total = longest(types[0]) * std::max<uint64_t>(1, replacement > 0 ? replacement - 1 : 0);
  } else if (name == "LPAD" || name == "RPAD") {
    // The length asked for; a negative one reads as past any limit.
    const auto n = constant(1);
    if (!n || !*n) {
      unbounded = true;
    } else {
      total = **n < 0 ? past_packet : std::min(static_cast<uint64_t>(**n), past_packet);
    }
  } else if (name == "REPEAT") {
    // The string times the count; a negative count reads as past any limit.
    const auto n = constant(1);
    const uint64_t each = longest(types[0]);
    if (!n || !*n) {
      unbounded = true;
    } else if (**n < 0) {
      total = each == 0 ? 0 : past_packet;
    } else {
      const auto count = static_cast<uint64_t>(**n);
      total = each != 0 && count > past_packet / each ? past_packet : each * count;
    }
  } else {
    // A piece of the string: what a constant position or length takes
    // away (nothing more once one is NULL).
    total = longest(types[0]);
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
  const uint64_t per_character = binary ? 1 : _settings.result_max_bytes;
  const uint64_t bytes = std::min(total, past_packet) * per_character;
  ResultType& t = m.type;
  t.collation = binary ? binary_collation : _settings.result_collation;
  t.decimals = protocol::not_fixed_decimals;
  t.flags = static_cast<uint16_t>(binary || is_bin_collation(m.derivation.first) ? c::binary : 0);
  if (unbounded) {
    t.field_type = f::long_blob;
    t.length = static_cast<uint32_t>(std::min<uint64_t>(max_allowed_packet * per_character, UINT32_MAX));
  } else if (bytes > max_varchar_bytes) {
    // A BLOB, whose text length is counted in characters once more
    // (observed), and at most max_allowed_packet.
    t.field_type = bytes > max_medium_blob_bytes ? f::long_blob : f::medium_blob;
    t.length = static_cast<uint32_t>(std::min(bytes * per_character, max_allowed_packet));
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
  // Whether the result counts bytes rather than characters.
  const auto in_bytes = [&]() { return string_function(e)->type.collation == binary_collation; };
  if (is_trim(name)) {
    // Whole runs of `remstr` (a space by default), matched byte for byte.
    const auto s = text(*e.args[0]);
    if (!s) return {};
    std::string remove = " ";
    if (e.args.size() == 2) {
      const auto r = text(*e.args[1]);
      if (!r) return {};
      remove = *r;
    }
    if (remove.empty()) return *s;
    const bool leading = name != "RTRIM" && e.scope != "TRAILING", trailing = name != "LTRIM" && e.scope != "LEADING";
    size_t begin = 0, end = s->size();
    if (leading)
      while (end - begin >= remove.size() && s->compare(begin, remove.size(), remove) == 0) begin += remove.size();
    if (trailing)
      while (end - begin >= remove.size() && s->compare(end - remove.size(), remove.size(), remove) == 0) end -= remove.size();
    return s->substr(begin, end - begin);
  }
  if (name == "REPLACE") {
    const auto s = text(*e.args[0]);
    if (!s) return {};
    const auto from = text(*e.args[1]);
    if (!from) return {};
    const auto to = text(*e.args[2]);
    if (!to) return {};
    if (from->empty()) return *s;
    std::string out;
    size_t at = 0;
    for (size_t found = s->find(*from); found != std::string::npos; found = s->find(*from, at)) {
      out.append(*s, at, found - at);
      out += *to;
      at = found + from->size();
    }
    out.append(*s, at, std::string::npos);
    return out;
  }
  if (name == "REVERSE") {
    const auto s = text(*e.args[0]);
    if (!s) return {};
    const auto starts = character_starts(*s, in_bytes());
    std::string out;
    for (size_t i = starts.size() - 1; i > 0; --i) out.append(*s, starts[i - 1], starts[i] - starts[i - 1]);
    return out;
  }
  if (is_locate(name)) {
    // LOCATE(substr, str[, pos]), POSITION(substr IN str), INSTR(str, substr).
    const Expr& needle_expr = name == "INSTR" ? *e.args[1] : *e.args[0];
    const Expr& haystack_expr = name == "INSTR" ? *e.args[0] : *e.args[1];
    const auto needle = text(needle_expr);
    const auto haystack = text(haystack_expr);
    int64_t from = 1;
    if (e.args.size() == 3) {
      const auto n = integer_argument(*e.args[2]);
      if (!n) return {};
      from = *n;
    }
    if (!needle || !haystack) return {};
    if (from < 1) return int64_t{0};
    // Compared under the two strings' collation; byte for byte when either
    // is binary, or when their collations do not agree (observed: no
    // error).
    const auto derived = [&](const Expr& x) -> Derivation {
      const auto t = type(x);
      if (t.field_type == f::null) return {binary_collation, ignorable_coercibility};
      if (is_string_type(t)) return derivation(x, t);
      return {_settings.result_collation, numeric_coercibility};
    };
    uint16_t collation = binary_collation;
    try {
      collation = aggregate_collations({derived(needle_expr), derived(haystack_expr)}, "locate").first;
    } catch (const SqlError&) {
    }
    const Collation* compare = collation_by_id(collation);
    if (!compare) throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'comparing strings under this collation'");
    const bool bytes = collation == binary_collation;
    const auto hay = character_starts(*haystack, bytes);
    const auto count = static_cast<int64_t>(hay.size()) - 1;
    const auto length = static_cast<int64_t>(character_starts(*needle, bytes).size()) - 1;
    if (length == 0) return from <= count + 1 ? from : int64_t{0};
    for (int64_t i = from - 1; i + length <= count; ++i) {
      const auto begin = hay[static_cast<size_t>(i)], end = hay[static_cast<size_t>(i + length)];
      if (compare->compare(std::string_view(*haystack).substr(begin, end - begin), *needle) == 0) return i + 1;
    }
    return int64_t{0};
  }
  if (name == "LPAD" || name == "RPAD") {
    const auto s = text(*e.args[0]);
    if (!s) return {};
    const auto n = integer_argument(*e.args[1]);
    if (!n) return {};
    const auto pad = text(*e.args[2]);
    if (!pad || *n < 0) return {};
    const bool bytes = in_bytes();
    const auto starts = character_starts(*s, bytes);
    const auto count = static_cast<int64_t>(starts.size()) - 1;
    if (*n <= count) return s->substr(0, starts[static_cast<size_t>(*n)]);
    if (pad->empty()) return std::string();
    if (static_cast<uint64_t>(*n) > max_allowed_packet) {
      _warnings.push_back(too_long(name));
      return {};
    }
    const auto pad_starts = character_starts(*pad, bytes);
    const auto pad_count = pad_starts.size() - 1;
    std::string fill;
    for (int64_t i = 0; i < *n - count; ++i) {
      const auto k = static_cast<size_t>(i) % pad_count;
      fill.append(*pad, pad_starts[k], pad_starts[k + 1] - pad_starts[k]);
    }
    std::string out = name == "LPAD" ? fill + *s : *s + fill;
    if (out.size() > max_allowed_packet) {
      _warnings.push_back(too_long(name));
      return {};
    }
    return out;
  }
  if (name == "REPEAT") {
    const auto s = text(*e.args[0]);
    if (!s) return {};
    const auto n = integer_argument(*e.args[1]);
    if (!n) return {};
    if (*n <= 0 || s->empty()) return std::string();
    if (static_cast<uint64_t>(*n) > max_allowed_packet / s->size()) {
      _warnings.push_back(too_long(name));
      return {};
    }
    std::string out;
    out.reserve(s->size() * static_cast<size_t>(*n));
    for (int64_t i = 0; i < *n; ++i) out += *s;
    return out;
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

namespace {

bool is_integer_field(const ResultType& t) {
  return !t.number_string &&
         (t.field_type == f::longlong || t.field_type == f::long_ || t.field_type == f::int24 || t.field_type == f::short_ || t.field_type == f::tiny);
}

// A DECIMAL rounded (half away from zero) or truncated to a multiple of
// 10^k, k > 0.
Decimal to_tens(const Decimal& x, int64_t k, bool round) {
  std::string digits = x.truncated(0).to_string();
  const bool negative = !digits.empty() && digits[0] == '-';
  if (negative) digits.erase(0, 1);
  if (k > static_cast<int64_t>(digits.size())) return Decimal::from_int(0);
  const auto keep_length = digits.size() - static_cast<size_t>(k);
  std::string keep = digits.substr(0, keep_length);
  if (round && digits[keep_length] >= '5') {
    // Add one to the kept digits.
    size_t i = keep.size();
    while (i > 0 && keep[i - 1] == '9') keep[--i] = '0';
    if (i == 0) {
      keep.insert(keep.begin(), '1');
    } else {
      ++keep[i - 1];
    }
  }
  if (keep.empty()) return Decimal::from_int(0);
  return *Decimal::parse((negative ? "-" : "") + keep + std::string(static_cast<size_t>(k), '0'));
}

// No negative zero.
Decimal unsigned_zero(const Decimal& d) { return d.is_zero() && d.negative() ? d.negated() : d; }

}  // namespace

std::optional<ResultType> Evaluator::number_function(const Expr& e) const {
  if (e.kind != sql::ExprKind::call) return std::nullopt;
  const auto& name = e.value;
  size_t least = 1, most = 1;
  if (name == "ROUND") {
    most = 2;
  } else if (name == "TRUNCATE") {
    least = most = 2;
  } else if (name != "ABS" && name != "SIGN" && name != "FLOOR" && name != "CEILING" && name != "CEIL") {
    return std::nullopt;
  }
  if (e.args.size() < least || e.args.size() > most) wrong_count(name);
  const auto a = type(*e.args[0]);
  const bool not_null = std::all_of(e.args.begin(), e.args.end(), [&](const sql::ExprPtr& x) { return !type(*x).nullable(); });
  const bool integer = is_integer_field(a), decimal = a.field_type == f::newdecimal;
  // A BIGINT of 20 digits, whether signed or not (observed).
  const auto bigint = [&](bool is_unsigned) {
    auto t = integer_type(20, is_unsigned, not_null);
    t.length = 21;
    return t;
  };
  if (name == "SIGN") return integer_type(20, false, not_null);
  if (name == "ABS") {
    if (integer) {
      auto t = integer_type(a.precision, a.is_unsigned(), not_null);
      t.length = a.length;
      return t;
    }
    if (decimal) return decimal_type(a.precision, a.decimals, not_null);
    // NULL and a hex literal: a DOUBLE of 17, no decimals (observed).
    if (a.field_type == f::null || a.number_string) {
      auto t = double_type(17, not_null);
      t.decimals = 0;
      if (a.number_string) t.flags = static_cast<uint16_t>(t.flags | c::unsigned_);
      return t;
    }
    return double_type(23, not_null);
  }
  if (name == "FLOOR" || name == "CEILING" || name == "CEIL") {
    if (integer) return bigint(a.is_unsigned());
    if (decimal) {
      // A BIGINT while 18 digits hold the result (one more for a carry).
      const int digits = a.precision - a.decimals + 1;
      if (digits <= 18) return bigint(false);
      return decimal_type(std::min(digits, static_cast<int>(decimal_max_precision)), 0, not_null);
    }
    return double_type(23, not_null);
  }
  // ROUND and TRUNCATE.
  if (integer) return bigint(a.is_unsigned());
  if (!decimal) return double_type(23, not_null);
  const auto places = e.args.size() == 2 ? constant_integer(*e.args[1]) : std::optional<std::optional<int64_t>>(int64_t{0});
  // Decimals that are not a constant: the argument's own scale.
  if (!places) return decimal_type(a.precision, a.decimals, not_null);
  const int scale = *places ? static_cast<int>(std::clamp<int64_t>(**places, 0, a.decimals)) : 0;
  // ROUND keeps a digit for its carry, when it drops any (observed).
  const bool drops = !*places || **places < a.decimals;
  const int whole = a.precision - a.decimals + (name == "ROUND" && drops ? 1 : 0);
  return decimal_type(std::min(whole + scale, static_cast<int>(decimal_max_precision)), scale, not_null);
}

Value Evaluator::number_function_value(const Expr& e) {
  const auto t = *number_function(e);
  const auto& name = e.value;
  const auto at = type(*e.args[0]);
  const auto v = value(*e.args[0]);
  if (is_null(v)) return {};
  const auto* signed_ = std::get_if<int64_t>(&v);
  const auto* unsigned_ = std::get_if<uint64_t>(&v);
  const auto* exact = std::get_if<Decimal>(&v);
  const bool integer = is_integer_field(at);
  if (name == "SIGN") {
    if (signed_) return int64_t{*signed_ > 0 ? 1 : *signed_ < 0 ? -1 : 0};
    if (unsigned_) return int64_t{*unsigned_ > 0 ? 1 : 0};
    if (exact) return int64_t{exact->is_zero() ? 0 : exact->negative() ? -1 : 1};
    const double r = real(v, at);
    return int64_t{r > 0 ? 1 : r < 0 ? -1 : 0};
  }
  if (name == "ABS") {
    if (t.field_type == f::double_) return std::fabs(real(v, at));
    if (exact) return exact->negative() ? exact->negated() : *exact;
    if (signed_) {
      if (*signed_ == std::numeric_limits<int64_t>::min())
        throw SqlError(er_data_out_of_range, "22003", "BIGINT value is out of range in '" + mysql_text(e) + "'");
      return *signed_ < 0 ? -*signed_ : *signed_;
    }
    return v;
  }
  if (name == "FLOOR" || name == "CEILING" || name == "CEIL") {
    const bool floor = name == "FLOOR";
    if (t.field_type == f::double_) return floor ? std::floor(real(v, at)) : std::ceil(real(v, at));
    if (!exact) return v;
    auto whole = exact->truncated(0);
    if (!(whole == *exact)) {
      if (floor && exact->negative()) whole = whole - Decimal::from_int(1);
      if (!floor && !exact->negative()) whole = whole + Decimal::from_int(1);
    }
    whole = unsigned_zero(whole);
    if (t.field_type == f::newdecimal) return whole;
    return whole.to_int().value_or(0);
  }
  // ROUND and TRUNCATE.
  const bool round = name == "ROUND";
  int64_t places = 0;
  if (e.args.size() == 2) {
    const auto p = integer_argument(*e.args[1]);
    if (!p) return {};
    places = *p;
  }
  if (t.field_type == f::double_) {
    const double x = real(v, at);
    // Half to even, as MySQL's doubles round.
    const auto apply = [&](double y) { return round ? std::nearbyint(y) : std::trunc(y); };
    if (places > 308 || places < -308) return places > 0 ? x : 0.0;
    const double scale = std::pow(10.0, static_cast<double>(places < 0 ? -places : places));
    const double r = places >= 0 ? apply(x * scale) / scale : apply(x / scale) * scale;
    return std::isfinite(r) ? r : x;
  }
  if (integer) {
    if (places >= 0) return v;
    // To a multiple of 10^-places, half away from zero.
    if (places <= -20) return signed_ ? Value(int64_t{0}) : Value(uint64_t{0});
    uint64_t p = 1;
    for (int64_t i = 0; i < -places; ++i) p *= 10;
    if (unsigned_) {
      uint64_t q = *unsigned_ / p;
      if (round && *unsigned_ % p >= (p + 1) / 2) ++q;
      return q * p;
    }
    const int64_t x = *signed_;
    const auto ip = static_cast<int64_t>(p);
    int64_t q = x / ip;
    const int64_t r = x % ip;
    if (round && (r < 0 ? -r : r) * 2 >= ip) q += x < 0 ? -1 : 1;
    return q * ip;
  }
  const Decimal x = exact ? *exact : Decimal::from_int(0);
  if (places < 0) return unsigned_zero(to_tens(x, -places, round));
  const int scale = static_cast<int>(std::min<int64_t>(places, decimal_max_scale));
  return unsigned_zero(round ? x.rounded(scale) : x.truncated(scale));
}

}  // namespace kvasir::exec
