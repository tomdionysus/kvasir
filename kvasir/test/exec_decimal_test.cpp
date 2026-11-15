//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual, "Precision Math": exact-value arithmetic for DECIMAL; a
// sum's scale is the larger operand scale, a product's the sum of the
// scales, a quotient's the dividend's plus div_precision_increment (4);
// rounding is half away from zero; at most 65 digits and 30 after the point
// in a column. The expected values below were observed on MySQL 8.4.13
// (Oracle's container on node-b, 2026-11-15), including the internal
// nine-digit carrying of quotients and how doubles are shown.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "kvasir/exec/decimal.h"

namespace kvasir::exec {
namespace {

Decimal d(std::string_view text) {
  auto value = Decimal::parse(text);
  EXPECT_TRUE(value) << text;
  return value.value_or(Decimal());
}

std::string quotient(std::string_view a, std::string_view b, int shown) {
  const auto q = Decimal::divide(d(a), d(b), 4);
  if (!q) return "NULL";
  return q->rounded(shown).to_string();
}

TEST(ExecDecimal, LiteralsKeepTheirDigits) {
  EXPECT_EQ(d("1.5").to_string(), "1.5");
  EXPECT_EQ(d("-1.5").to_string(), "-1.5");
  EXPECT_EQ(d(".5").to_string(), "0.5");
  EXPECT_EQ(d("5.").to_string(), "5");
  EXPECT_EQ(d("1.50").to_string(), "1.50");
  EXPECT_EQ(d("0.0").to_string(), "0.0");
  EXPECT_EQ(d("007").to_string(), "7");
  EXPECT_EQ(d("123456789012345678901234567890.5").to_string(), "123456789012345678901234567890.5");
  EXPECT_EQ(d("18446744073709551616").to_string(), "18446744073709551616");
  EXPECT_EQ(d("1.50").frac(), 2);
  EXPECT_EQ(d(".5").int_digits(), 1);
  EXPECT_EQ(d("123.4").int_digits(), 3);
}

TEST(ExecDecimal, ZeroHasNoSign) {
  EXPECT_EQ(d("-0.00").to_string(), "0.00");
  EXPECT_FALSE(d("-0").negative());
  EXPECT_EQ((d("1.5") - d("1.5")).to_string(), "0.0");
}

TEST(ExecDecimal, TextThatIsNotADecimalIsRefused) {
  for (const char* text : {"", ".", "-", "-.", "1e3", "1.2.3", "+1", " 1", "1 ", "0x1", "1,5", "--1"}) EXPECT_FALSE(Decimal::parse(text)) << text;
}

TEST(ExecDecimal, FromIntegers) {
  EXPECT_EQ(Decimal::from_int(-9223372036854775807 - 1).to_string(), "-9223372036854775808");
  EXPECT_EQ(Decimal::from_uint(18446744073709551615u).to_string(), "18446744073709551615");
  EXPECT_EQ(Decimal::from_int(0).to_string(), "0");
}

TEST(ExecDecimal, Sums) {
  EXPECT_EQ((d("1.5") + d("2.25")).to_string(), "3.75");
  EXPECT_EQ((d("1.5") - d("2.25")).to_string(), "-0.75");
  EXPECT_EQ((d("1") + d("1.5")).to_string(), "2.5");
  EXPECT_EQ((d("-1.5") + d("-2.25")).to_string(), "-3.75");
  EXPECT_EQ((d("999999999.999999999") + d("0.000000001")).to_string(), "1000000000.000000000");
  EXPECT_EQ((d(std::string(65, '9')) + d("1")).to_string(), "1" + std::string(65, '0'));
}

TEST(ExecDecimal, Products) {
  EXPECT_EQ((d("1.5") * d("2.25")).to_string(), "3.375");
  EXPECT_EQ((d("-1.5") * d("2")).to_string(), "-3.0");
  // Past 30 digits after the point the product is rounded to 30.
  EXPECT_EQ((d("1.000000000000000000000000000001") * d("1.5")).to_string(), "1.500000000000000000000000000002");
  EXPECT_EQ((d("123456789.123456789") * d("987654321.987654321")).to_string(), "121932631356500531.347203169112635269");
}

TEST(ExecDecimal, QuotientsCarryWholeWords) {
  EXPECT_EQ(quotient("1", "3", 4), "0.3333");
  EXPECT_EQ(quotient("10", "4", 4), "2.5000");
  EXPECT_EQ(quotient("1.5", "2.25", 5), "0.66667");
  EXPECT_EQ(Decimal::divide(d("1"), d("3"), 4)->to_string(), "0.333333333");
  const auto third = *Decimal::divide(d("1"), d("3"), 4);
  const auto ninth = *Decimal::divide(third, d("3"), 4);
  const auto twenty_seventh = *Decimal::divide(ninth, d("3"), 4);
  EXPECT_EQ(twenty_seventh.rounded(12).to_string(), "0.037037037000");
  EXPECT_EQ(quotient("-7", "2", 4), "-3.5000");
}

TEST(ExecDecimal, DivisionByZeroIsNothing) {
  EXPECT_FALSE(Decimal::divide(d("1"), d("0"), 4));
  EXPECT_FALSE(Decimal::divide(d("1.5"), d("0.00"), 4));
  EXPECT_FALSE(Decimal::remainder(d("1"), d("0")));
}

TEST(ExecDecimal, Remainders) {
  EXPECT_EQ(Decimal::remainder(d("7.5"), d("2"))->to_string(), "1.5");
  EXPECT_EQ(Decimal::remainder(d("-7"), d("3"))->to_string(), "-1");
  EXPECT_EQ(Decimal::remainder(d("7"), d("-3"))->to_string(), "1");
  EXPECT_EQ(Decimal::remainder(d("10.25"), d("0.5"))->to_string(), "0.25");
}

TEST(ExecDecimal, RoundingIsHalfAwayFromZero) {
  EXPECT_EQ(d("2.5").rounded(0).to_string(), "3");
  EXPECT_EQ(d("-2.5").rounded(0).to_string(), "-3");
  EXPECT_EQ(d("2.45").rounded(1).to_string(), "2.5");
  EXPECT_EQ(d("2.44").rounded(1).to_string(), "2.4");
  EXPECT_EQ(d("9.99").rounded(1).to_string(), "10.0");
  EXPECT_EQ(d("1.5").rounded(3).to_string(), "1.500");
  EXPECT_EQ(d("2.59").truncated(1).to_string(), "2.5");
  EXPECT_EQ(d("-2.59").truncated(0).to_string(), "-2");
}

TEST(ExecDecimal, Comparison) {
  EXPECT_EQ(compare(d("1.5"), d("1.50")), 0);
  EXPECT_EQ(compare(d("-1"), d("0.5")), -1);
  EXPECT_EQ(compare(d("-1"), d("-2")), 1);
  EXPECT_EQ(compare(d("0.0"), d("-0")), 0);
  EXPECT_TRUE(d("10") == d("10.000"));
}

TEST(ExecDecimal, Conversions) {
  EXPECT_EQ(d("1.5").to_double(), 1.5);
  EXPECT_EQ(d("-0.125").to_double(), -0.125);
  EXPECT_EQ(d("12.9").to_int(), 12);
  EXPECT_EQ(d("-12.9").to_int(), -12);
  EXPECT_FALSE(d("9223372036854775808").to_int());
  EXPECT_EQ(d("-9223372036854775808").to_int(), -9223372036854775807 - 1);
}

TEST(ExecDecimal, PastEightyOneDigitsIsAnOverflow) {
  const auto big = d(std::string(81, '9'));
  EXPECT_THROW(big + d("1"), std::overflow_error);
  EXPECT_THROW(big * d("10"), std::overflow_error);
}

TEST(ExecDouble, MySqlsTextForDoubles) {
  EXPECT_EQ(format_double(1000), "1000");
  EXPECT_EQ(format_double(1.5e-3), "0.0015");
  EXPECT_EQ(format_double(-1e308), "-1e308");
  EXPECT_EQ(format_double(1e14), "100000000000000");
  EXPECT_EQ(format_double(1e15), "1e15");
  EXPECT_EQ(format_double(-1e15), "-1e15");
  EXPECT_EQ(format_double(1e16), "1e16");
  EXPECT_EQ(format_double(123456789012345.0), "123456789012345");
  EXPECT_EQ(format_double(1234567890123456.0), "1.234567890123456e15");
  EXPECT_EQ(format_double(12345678901234567.0), "1.2345678901234568e16");
  EXPECT_EQ(format_double(1e-5), "0.00001");
  EXPECT_EQ(format_double(1.5e-7), "0.00000015");
  EXPECT_EQ(format_double(1e-15), "0.000000000000001");
  EXPECT_EQ(format_double(1e-16), "1e-16");
  EXPECT_EQ(format_double(-1.5e-17), "-1.5e-17");
  EXPECT_EQ(format_double(1.5e-20), "1.5e-20");
  EXPECT_EQ(format_double(4.9e-324), "5e-324");
  EXPECT_EQ(format_double(1e100), "1e100");
  EXPECT_EQ(format_double(1.7976931348623157e308), "1.7976931348623157e308");
  EXPECT_EQ(format_double(1.0 / 3.0), "0.3333333333333333");
  EXPECT_EQ(format_double(0.1 + 0.2), "0.30000000000000004");
  EXPECT_EQ(format_double(3.14159), "3.14159");
  EXPECT_EQ(format_double(123.456), "123.456");
  EXPECT_EQ(format_double(1.0), "1");
  EXPECT_EQ(format_double(0.0), "0");
  EXPECT_EQ(format_double(-0.0), "-0");
}

}  // namespace
}  // namespace kvasir::exec
