//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kvasir/exec/decimal.h"

// Values as SQL computes with them, the type a result column reports for
// them, and the errors and warnings computing them can give.
namespace kvasir::exec {

// NULL, a signed or unsigned integer, an exact decimal, a double, or a
// string's bytes (its character set is its type's).
using Value = std::variant<std::monostate, int64_t, uint64_t, Decimal, double, std::string>;

inline bool is_null(const Value& v) noexcept { return std::holds_alternative<std::monostate>(v); }

// What a result column says of itself in its definition, and the digits of
// precision that typing arithmetic on it needs.
struct ResultType {
  uint8_t field_type = 0;
  uint16_t collation = 0;
  uint32_t length = 0;
  uint8_t decimals = 0;
  uint16_t flags = 0;
  // Digits: of an integer, or of a decimal in all.
  int precision = 0;
  // A decimal shown at its value's own scale, not the column's: a user
  // variable holding a DECIMAL reports 30 decimals and shows 1.5 as 1.5.
  bool own_scale = false;
  // A hex or bit literal: an unsigned integer in a numeric context, where
  // another binary string (an unset user variable) is read as a number's text.
  bool number_string = false;

  bool nullable() const noexcept;
  bool is_unsigned() const noexcept;
};

// A value as the text protocol carries it for a column of `type`: a decimal
// at the column's scale, a double as MySQL shows it. Nothing for NULL.
std::optional<std::string> to_text(const Value& value, const ResultType& type);

// What SHOW WARNINGS lists.
struct Warning {
  std::string level;  // "Note", "Warning" or "Error"
  uint16_t code = 0;
  std::string message;
};

// A statement could not be carried out: its MySQL error, and the warnings
// raised before it, which SHOW WARNINGS lists ahead of it (observed: a DATE
// compared with 'abc' warns 1292, then fails 1525).
class SqlError : public std::runtime_error {
 public:
  SqlError(uint16_t code, std::string sqlstate, const std::string& message, std::vector<Warning> before = {})
      : std::runtime_error(message), _code(code), _sqlstate(std::move(sqlstate)), _before(std::move(before)) {}
  uint16_t code() const noexcept { return _code; }
  const std::string& sqlstate() const noexcept { return _sqlstate; }
  const std::vector<Warning>& before() const noexcept { return _before; }

 private:
  uint16_t _code;
  std::string _sqlstate;
  std::vector<Warning> _before;
};

inline constexpr uint16_t er_wrong_arguments = 1210;
inline constexpr uint16_t er_not_supported_yet = 1235;
inline constexpr uint16_t er_division_by_zero = 1365;
inline constexpr uint16_t er_truncated_wrong_value = 1292;
inline constexpr uint16_t er_wrong_value_for_type = 1411;
inline constexpr uint16_t er_data_out_of_range = 1690;
inline constexpr uint16_t er_bad_field_error = 1054;
inline constexpr uint16_t er_ambiguous_field = 1052;
inline constexpr uint16_t er_invalid_group_func_use = 1111;

}  // namespace kvasir::exec
