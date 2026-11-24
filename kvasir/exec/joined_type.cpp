//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The type a set operation's result column takes over its parts' columns,
// as MySQL 8.4.13 makes a temporary table's field for it (observed over
// every pair of column types: TODO/2026-11-24-set-operations-notes.md and
// build/union-matrix.txt).

#include <algorithm>
#include <cstdint>
#include <vector>

#include "kvasir/exec/expression.h"
#include "kvasir/protocol/types.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;

enum class Family : uint8_t { null, integer, year, decimal, real, temporal, string };

Family family_of(const ResultType& t) {
  switch (t.field_type) {
    case f::null:
      return Family::null;
    case f::tiny:
    case f::short_:
    case f::int24:
    case f::long_:
    case f::longlong:
      return t.number_string ? Family::string : Family::integer;
    case f::year:
      return Family::year;
    case f::decimal:
    case f::newdecimal:
      return Family::decimal;
    case f::float_:
    case f::double_:
      return Family::real;
    case f::date:
    case f::time:
    case f::datetime:
    case f::timestamp:
      return Family::temporal;
    default:
      return Family::string;
  }
}

// The integer types from narrowest; one past LONGLONG is a DECIMAL.
constexpr uint8_t integer_types[] = {f::tiny, f::short_, f::int24, f::long_, f::longlong};

int rank_of(uint8_t type) {
  for (int i = 0; i < 5; ++i)
    if (integer_types[i] == type) return i;
  return 4;
}

bool is_blob(const ResultType& t) {
  return t.field_type == f::blob || t.field_type == f::tiny_blob || t.field_type == f::medium_blob || t.field_type == f::long_blob;
}

// An integer's digits and its sign: its length as text.
uint32_t integer_chars(const ResultType& t) { return static_cast<uint32_t>(t.precision) + (t.is_unsigned() ? 0u : 1u); }

// The characters a part takes as text in a string result.
uint32_t text_chars(const ResultType& t) {
  switch (family_of(t)) {
    case Family::null:
      return 0;
    case Family::integer:
      return integer_chars(t);
    case Family::year:
      return 4;
    case Family::real:
      return t.field_type == f::float_ ? 12 : 22;
    case Family::decimal:
    case Family::temporal:
      return t.length;
    case Family::string:
      return t.collation == protocol::collation::binary ? t.length : t.length / widest_character(t.collation);
  }
  return t.length;
}

// Its bytes in a binary string result: a string's own, a FLOAT's or
// DOUBLE's length (observed: a 1e0 literal is short, a DOUBLE column 22),
// anything else its characters.
uint32_t text_bytes(const ResultType& t) {
  const auto family = family_of(t);
  return family == Family::string || family == Family::real ? t.length : text_chars(t);
}

}  // namespace

ResultType joined_type(const std::vector<ResultType>& parts, uint16_t collation) {
  ResultType out;
  out.collation = protocol::collation::binary;
  bool not_null = true;
  bool any[7] = {};
  for (const auto& p : parts) {
    any[static_cast<size_t>(family_of(p))] = true;
    if (!(p.flags & c::not_null)) not_null = false;
  }
  const auto has = [&](Family family) { return any[static_cast<size_t>(family)]; };
  uint16_t flags = not_null ? c::not_null : 0;
  const bool numbers = has(Family::integer) || has(Family::year) || has(Family::decimal) || has(Family::real);

  // NULL alone: a binary string of no length.
  if (!numbers && !has(Family::temporal) && !has(Family::string)) {
    out.field_type = f::var_string;
    out.flags = static_cast<uint16_t>(flags | c::binary);
    return out;
  }

  if (has(Family::temporal) && !numbers && !has(Family::string)) {
    // DATE with DATE, TIME with TIME, TIMESTAMP with TIMESTAMP keep their
    // type; any other two make DATETIME. The most fraction digits.
    uint8_t type = 0;
    bool mixed = false;
    int fsp = 0;
    for (const auto& p : parts) {
      if (family_of(p) != Family::temporal) continue;
      if (type && type != p.field_type) mixed = true;
      type = p.field_type;
      if (p.field_type != f::date) fsp = std::max<int>(fsp, p.decimals);
    }
    if (mixed) type = f::datetime;
    out.field_type = type;
    out.decimals = static_cast<uint8_t>(type == f::date ? 0 : fsp);
    const uint32_t fraction = out.decimals ? out.decimals + 1u : 0u;
    out.length = type == f::date ? 10 : type == f::time ? 10 + fraction : 19 + fraction;
    out.precision = type == f::date ? 8 : type == f::time ? 7 + out.decimals : 14 + out.decimals;
    flags |= c::binary;
    // A date's field is flagged so, a TIME's not (observed).
    if (not_null && type != f::time) flags |= c::no_default_value;
    out.flags = flags;
    return out;
  }

  if (has(Family::string) || has(Family::temporal)) {
    // A string as long as the longest part as text; binary if any string
    // part is; a TEXT or BLOB part makes a BLOB.
    bool binary = false, blob = false, every_char = true, any_string = false;
    uint32_t chars = 0, bytes = 0, blob_length = 0;
    for (const auto& p : parts) {
      if (family_of(p) == Family::string) {
        any_string = true;
        if (p.collation == protocol::collation::binary) binary = true;
        if (p.field_type != f::string) every_char = false;
        if (is_blob(p)) {
          blob = true;
          const uint64_t length = p.collation == protocol::collation::binary ? p.length : uint64_t{p.length} * 4;
          blob_length = static_cast<uint32_t>(std::max<uint64_t>(blob_length, std::min<uint64_t>(length, UINT32_MAX)));
        }
      }
      chars = std::max(chars, text_chars(p));
      bytes = std::max(bytes, text_bytes(p));
    }
    out.collation = binary ? protocol::collation::binary : collation;
    if (blob) {
      out.field_type = f::blob;
      out.length = blob_length;
      flags |= c::blob;
    } else {
      out.field_type = any_string && every_char ? f::string : f::var_string;
      out.length = binary ? bytes : chars * widest_character(collation);
    }
    // Binary, or under a _bin collation (observed: 'a' COLLATE utf8mb4_bin).
    if (binary || is_bin_collation(out.collation)) flags |= c::binary;
    out.flags = flags;
    return out;
  }

  flags |= c::num;
  if (has(Family::year) && !has(Family::integer) && !has(Family::decimal) && !has(Family::real)) {
    out.field_type = f::year;
    out.length = 4;
    out.precision = 4;
    out.flags = static_cast<uint16_t>(flags | c::unsigned_ | c::zerofill);
    return out;
  }

  if (has(Family::real)) {
    // The parts merged pair by pair, left to right (observed: FLOAT then
    // BIGINT is FLOAT, BIGINT then FLOAT DOUBLE): FLOAT stays FLOAT beside
    // FLOAT, YEAR and the narrower integers, and after BIGINT; INT, DECIMAL
    // and DOUBLE make DOUBLE, as does BIGINT before it.
    enum class Seen : uint8_t { none, narrow, wide, longlong, floating, real };
    Seen seen = Seen::none;
    for (const auto& p : parts) {
      const auto family = family_of(p);
      Seen next = Seen::none;
      if (family == Family::null) continue;
      if (p.field_type == f::double_ || family == Family::decimal) next = Seen::real;
      if (p.field_type == f::float_) next = Seen::floating;
      if (family == Family::year || (family == Family::integer && p.field_type != f::long_ && p.field_type != f::longlong)) next = Seen::narrow;
      if (family == Family::integer && p.field_type == f::long_) next = Seen::wide;
      if (family == Family::integer && p.field_type == f::longlong) next = Seen::longlong;
      if (seen == Seen::none) {
        seen = next;
      } else if (seen == Seen::real || next == Seen::real) {
        seen = Seen::real;
      } else if (seen == Seen::floating) {
        seen = next == Seen::wide ? Seen::real : Seen::floating;
      } else if (next == Seen::floating) {
        seen = seen == Seen::narrow ? Seen::floating : Seen::real;
      } else {
        seen = std::max(seen, next);
      }
    }
    out.field_type = seen == Seen::floating ? f::float_ : f::double_;
    out.length = 23;
    out.decimals = protocol::not_fixed_decimals;
    out.flags = flags;
    return out;
  }

  // Integers and DECIMALs. An unsigned integer beside a signed one counts
  // as the next wider type (a BIGINT UNSIGNED as a DECIMAL).
  bool all_unsigned = true;
  for (const auto& p : parts)
    if (family_of(p) == Family::integer && !p.is_unsigned()) all_unsigned = false;
  if (has(Family::decimal)) all_unsigned = false;
  int rank = -1;
  int digits = 0, scale = 0;
  uint32_t length = 0;
  for (const auto& p : parts) {
    const auto family = family_of(p);
    if (family == Family::integer) {
      rank = std::max(rank, rank_of(p.field_type) + (p.is_unsigned() && !all_unsigned ? 1 : 0));
      digits = std::max(digits, p.precision);
      length = std::max(length, integer_chars(p));
    } else if (family == Family::year) {
      digits = std::max(digits, 4);
      length = std::max(length, 4u);
    } else if (family == Family::decimal) {
      digits = std::max(digits, p.precision - p.decimals);
      scale = std::max<int>(scale, p.decimals);
    }
  }
  if (has(Family::decimal) || rank > 4) {
    const int precision = std::min(digits + scale, 65);
    out.field_type = f::newdecimal;
    out.precision = precision;
    out.decimals = static_cast<uint8_t>(scale);
    out.length = static_cast<uint32_t>(precision + (scale > 0 ? 1 : 0) + 1);
    out.flags = flags;
    return out;
  }
  out.field_type = integer_types[std::max(rank, 0)];
  out.precision = digits;
  out.length = length;
  if (all_unsigned) flags |= c::unsigned_;
  out.flags = flags;
  return out;
}

}  // namespace kvasir::exec
