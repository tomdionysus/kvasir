//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The string functions, typed and evaluated as MySQL 8.4.13 does (the
// manual's "String Functions and Operators", as observed).

#include <algorithm>
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

}  // namespace

std::optional<Evaluator::Merged> Evaluator::string_function(const Expr& e) const {
  if (e.kind != sql::ExprKind::call) return std::nullopt;
  const auto& name = e.value;
  const bool concat = name == "CONCAT", concat_ws = name == "CONCAT_WS", length = is_length(name);
  if (!concat && !concat_ws && !length) return std::nullopt;
  if ((concat && e.args.empty()) || (concat_ws && e.args.size() < 2) || (length && e.args.size() != 1)) wrong_count(name);
  Merged m;
  if (length) {
    m.type = integer_type(9, false, !type(*e.args[0]).nullable());
    m.derivation = {binary_collation, numeric_coercibility};
    return m;
  }
  // The arguments' collations aggregated: a number's is the connection's at
  // NUMERIC coercibility, NULL's binary at IGNORABLE.
  std::vector<ResultType> types;
  std::vector<Derivation> collations;
  for (const auto& a : e.args) {
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
  uint64_t total = 0;
  for (size_t i = concat_ws ? 1 : 0; i < types.size(); ++i) total += longest(types[i]);
  // CONCAT_WS: a separator between each two arguments, NULL or not.
  if (concat_ws) total += longest(types[0]) * (types.size() - 2);
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
