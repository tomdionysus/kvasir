//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Exact decimal numbers, as MySQL's DECIMAL computes them (the 8.4 manual,
// "Precision Math"), written by hand. A value is a sign, an unscaled magnitude
// and a count of digits after the point; it keeps the digits it was written
// or computed with (1.50 stays 1.50), and is rounded only when asked.
//
// Division follows MySQL's own arithmetic, observed on MySQL 8.4.13: the
// quotient is carried to whole nine-digit words, past the increment asked
// for, and truncated there; the result column's scale is applied when it is
// shown. So 1/3/3/3 shows 0.037037037000, not 0.037037037037.
namespace kvasir::exec {

// MySQL's limits: 65 digits of precision and 30 after the point in a
// column; arithmetic carries up to 81 digits (nine words of nine).
inline constexpr int decimal_max_precision = 65;
inline constexpr int decimal_max_scale = 30;
inline constexpr int decimal_max_digits = 81;

class Decimal {
 public:
  Decimal() = default;

  // [-]digits[.digits], at least one digit, as a literal is written.
  // Nothing for anything else.
  static std::optional<Decimal> parse(std::string_view text);
  static Decimal from_int(int64_t value);
  static Decimal from_uint(uint64_t value);

  bool negative() const noexcept { return _negative; }
  bool is_zero() const noexcept;
  // Digits after the point.
  int frac() const noexcept { return _frac; }
  // Digits before the point, at least 1 (0.5 has one: its 0).
  int int_digits() const;

  // At its own scale: 0.5, 1.50, -3, 100000.
  std::string to_string() const;
  // Rounded half away from zero to `frac` digits, or padded with zeros.
  Decimal rounded(int frac) const;
  Decimal truncated(int frac) const;
  Decimal negated() const;

  // Each throws std::overflow_error past decimal_max_digits. A product's
  // scale is the sum of its operands', rounded to decimal_max_scale.
  friend Decimal operator+(const Decimal& a, const Decimal& b);
  friend Decimal operator-(const Decimal& a, const Decimal& b);
  friend Decimal operator*(const Decimal& a, const Decimal& b);
  // MySQL's division with `increment` digits asked for past the dividend's
  // scale (div_precision_increment, 4 by default). Nothing for a zero divisor.
  static std::optional<Decimal> divide(const Decimal& a, const Decimal& b, int increment);
  // The remainder, with the dividend's sign; nothing for a zero divisor.
  static std::optional<Decimal> remainder(const Decimal& a, const Decimal& b);

  // -1, 0 or 1.
  friend int compare(const Decimal& a, const Decimal& b);
  friend bool operator==(const Decimal& a, const Decimal& b) { return compare(a, b) == 0; }

  double to_double() const;
  // The integer part, if it fits.
  std::optional<int64_t> to_int() const;

 private:
  bool _negative = false;
  // The unscaled magnitude in base 10^9, least significant word first; empty
  // is zero.
  std::vector<uint32_t> _words;
  int _frac = 0;
};

// A double as MySQL shows it: the shortest digits that read back the same,
// in fixed notation from 1e-15 to below 1e15 and in exponent notation
// otherwise (1e15, 1.2345678901234568e16, 1e-16); -0 for negative zero.
std::string format_double(double value);

}  // namespace kvasir::exec
