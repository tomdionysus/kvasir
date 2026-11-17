//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Every expected type, length, decimals, flags, value, warning and error
// here was observed on MySQL 8.4.13 (Oracle's container on node-b,
// 2026-11-15) with a utf8mb4 client and `mysql --column-type-info`: the
// MySQL 8.4 manual's "Numeric Literals", "Precision Math", "Type Conversion
// in Expression Evaluation", "Arithmetic Operators", "Bit Functions and
// Operators", "Comparison Functions and Operators" and "Logical Operators",
// as the server applies them.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kvasir/exec/expression.h"
#include "kvasir/protocol/types.h"
#include "kvasir/sql/parser.h"

namespace kvasir::exec {
namespace {

std::string type_name(uint8_t t) {
  namespace f = protocol::field_type;
  switch (t) {
    case f::longlong:
      return "LONGLONG";
    case f::newdecimal:
      return "NEWDECIMAL";
    case f::double_:
      return "DOUBLE";
    case f::var_string:
      return "VAR_STRING";
    case f::null:
      return "NULL";
    default:
      return "type " + std::to_string(t);
  }
}

// As the mysql client lists them.
std::string flag_names(uint16_t flags) {
  namespace c = protocol::column_flag;
  std::string out;
  const auto add = [&](uint16_t bit, const char* name) {
    if (flags & bit) out += std::string(out.empty() ? "" : " ") + name;
  };
  add(c::not_null, "NOT_NULL");
  add(c::unsigned_, "UNSIGNED");
  add(c::binary, "BINARY");
  add(c::num, "NUM");
  return out;
}

struct Case {
  std::string sql;
  std::string type;
  uint16_t collation;
  uint32_t length;
  int decimals;
  std::string flags;
  std::string value;  // as the text protocol carries it; "NULL" for NULL
};

sql::Statement select(const std::string& sql) { return std::move(sql::parse("SELECT " + sql).at(0)); }

void check(const std::vector<Case>& cases) {
  for (const auto& c : cases) {
    SCOPED_TRACE(c.sql);
    const auto statement = select(c.sql);
    const auto& expr = *statement.select.items.at(0).expr;
    Evaluator evaluator;
    const auto type = evaluator.type(expr);
    EXPECT_EQ(type_name(type.field_type), c.type);
    EXPECT_EQ(type.collation, c.collation);
    EXPECT_EQ(type.length, c.length);
    EXPECT_EQ(type.decimals, c.decimals);
    EXPECT_EQ(flag_names(type.flags), c.flags);
    const auto text = to_text(evaluator.value(expr), type);
    EXPECT_EQ(text.value_or("NULL"), c.value);
  }
}

// Integer literals carry a place for a sign; past BIGINT they are UNSIGNED,
// past BIGINT UNSIGNED, DECIMAL.
TEST(ExecExpression, IntegerLiterals) {
  check({
      {"1", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "1"},
      {"-1", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "-1"},
      {"2147483648", "LONGLONG", 63, 11, 0, "NOT_NULL BINARY NUM", "2147483648"},
      {"-2147483648", "LONGLONG", 63, 11, 0, "NOT_NULL BINARY NUM", "-2147483648"},
      {"9223372036854775807", "LONGLONG", 63, 20, 0, "NOT_NULL BINARY NUM", "9223372036854775807"},
      {"9223372036854775808", "LONGLONG", 63, 19, 0, "NOT_NULL UNSIGNED BINARY NUM", "9223372036854775808"},
      {"-9223372036854775808", "LONGLONG", 63, 20, 0, "NOT_NULL BINARY NUM", "-9223372036854775808"},
      {"18446744073709551615", "LONGLONG", 63, 20, 0, "NOT_NULL UNSIGNED BINARY NUM", "18446744073709551615"},
      {"18446744073709551616", "NEWDECIMAL", 63, 21, 0, "NOT_NULL BINARY NUM", "18446744073709551616"},
      {"007", "LONGLONG", 63, 4, 0, "NOT_NULL BINARY NUM", "7"},
      {"TRUE", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"FALSE", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
  });
}

TEST(ExecExpression, DecimalAndDoubleLiterals) {
  check({
      {"1.5", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "1.5"},
      {"-1.5", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "-1.5"},
      {".5", "NEWDECIMAL", 63, 3, 1, "NOT_NULL BINARY NUM", "0.5"},
      {"5.", "NEWDECIMAL", 63, 2, 0, "NOT_NULL BINARY NUM", "5"},
      {"1.50", "NEWDECIMAL", 63, 5, 2, "NOT_NULL BINARY NUM", "1.50"},
      {"0.0", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "0.0"},
      {"123456789012345678901234567890.5", "NEWDECIMAL", 63, 33, 1, "NOT_NULL BINARY NUM", "123456789012345678901234567890.5"},
      {"1e3", "DOUBLE", 63, 3, 31, "NOT_NULL BINARY NUM", "1000"},
      {"1.5e-3", "DOUBLE", 63, 6, 31, "NOT_NULL BINARY NUM", "0.0015"},
      {"0.1e1", "DOUBLE", 63, 5, 31, "NOT_NULL BINARY NUM", "1"},
      {"-1e308", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "-1e308"},
  });
}

TEST(ExecExpression, StringHexBitAndNullLiterals) {
  check({
      {"'abc'", "VAR_STRING", 255, 12, 31, "NOT_NULL", "abc"},
      {"''", "VAR_STRING", 255, 0, 31, "NOT_NULL", ""},
      {"_latin1'x'", "VAR_STRING", 255, 4, 31, "NOT_NULL", "x"},
      {"N'n'", "VAR_STRING", 255, 4, 31, "NOT_NULL", "n"},
      // Observed 2026-11-17: a binary string stays one.
      {"_binary'ab'", "VAR_STRING", 63, 2, 31, "NOT_NULL BINARY", "ab"},
      {"0x41", "VAR_STRING", 63, 1, 0, "NOT_NULL UNSIGNED BINARY", "A"},
      {"X'4142'", "VAR_STRING", 63, 2, 0, "NOT_NULL UNSIGNED BINARY", "AB"},
      {"b'1'", "VAR_STRING", 63, 1, 0, "NOT_NULL BINARY", "\x01"},
      {"0b1000001", "VAR_STRING", 63, 1, 0, "NOT_NULL BINARY", "A"},
      {"NULL", "NULL", 63, 0, 0, "BINARY NUM", "NULL"},
  });
}

TEST(ExecExpression, IntegerArithmetic) {
  check({
      {"1+1", "LONGLONG", 63, 3, 0, "NOT_NULL BINARY NUM", "2"},
      {"1-2", "LONGLONG", 63, 3, 0, "NOT_NULL BINARY NUM", "-1"},
      {"2*3", "LONGLONG", 63, 3, 0, "NOT_NULL BINARY NUM", "6"},
      {"7 DIV 2", "LONGLONG", 63, 2, 0, "BINARY NUM", "3"},
      {"7 % 3", "LONGLONG", 63, 2, 0, "BINARY NUM", "1"},
      {"-7 % 3", "LONGLONG", 63, 2, 0, "BINARY NUM", "-1"},
      {"9223372036854775807+0", "LONGLONG", 63, 21, 0, "NOT_NULL BINARY NUM", "9223372036854775807"},
      {"18446744073709551615-1", "LONGLONG", 63, 21, 0, "NOT_NULL UNSIGNED BINARY NUM", "18446744073709551614"},
  });
}

TEST(ExecExpression, DecimalArithmetic) {
  check({
      {"1.5+2.25", "NEWDECIMAL", 63, 6, 2, "NOT_NULL BINARY NUM", "3.75"},
      {"1.5-2.25", "NEWDECIMAL", 63, 6, 2, "NOT_NULL BINARY NUM", "-0.75"},
      {"1.5*2.25", "NEWDECIMAL", 63, 7, 3, "NOT_NULL BINARY NUM", "3.375"},
      {"1+1.5", "NEWDECIMAL", 63, 5, 1, "NOT_NULL BINARY NUM", "2.5"},
      {"1.5/2.25", "NEWDECIMAL", 63, 10, 5, "BINARY NUM", "0.66667"},
      {"1/3", "NEWDECIMAL", 63, 7, 4, "BINARY NUM", "0.3333"},
      {"10/4", "NEWDECIMAL", 63, 8, 4, "BINARY NUM", "2.5000"},
      {"1/3/3/3", "NEWDECIMAL", 63, 15, 12, "BINARY NUM", "0.037037037000"},
      {"7.5 % 2", "NEWDECIMAL", 63, 4, 1, "BINARY NUM", "1.5"},
      {"7 % 2.5", "NEWDECIMAL", 63, 4, 1, "BINARY NUM", "2.0"},
      {"99999999999999999999999999999999999999999999999999999999999999999 + 1", "NEWDECIMAL", 63, 67, 0, "NOT_NULL BINARY NUM", "1" + std::string(65, '0')},
      {"1.000000000000000000000000000001 * 1.5", "NEWDECIMAL", 63, 35, 30, "NOT_NULL BINARY NUM", "1.500000000000000000000000000002"},
  });
}

TEST(ExecExpression, DoubleArithmetic) {
  check({
      {"1+1e0", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "2"},
      {"7e0 % 2", "DOUBLE", 63, 23, 31, "BINARY NUM", "1"},
      {"1/3e0", "DOUBLE", 63, 23, 31, "BINARY NUM", "0.3333333333333333"},
      {"0.1+0.2e0", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "0.30000000000000004"},
  });
}

TEST(ExecExpression, IntegerDivisionOfOtherTypes) {
  check({
      {"5 DIV 2.5", "LONGLONG", 63, 3, 0, "BINARY NUM", "2"},
      {"7.5 DIV 2", "LONGLONG", 63, 2, 0, "BINARY NUM", "3"},
      {"1e0 DIV 2", "LONGLONG", 63, 22, 0, "BINARY NUM", "0"},
  });
}

// A constant negative integer negated becomes DECIMAL, except the literal
// 9223372036854775808, whose negation is BIGINT's least value.
TEST(ExecExpression, Negation) {
  check({
      {"-18446744073709551615", "NEWDECIMAL", 63, 21, 0, "NOT_NULL BINARY NUM", "-18446744073709551615"},
      {"- 9223372036854775808", "LONGLONG", 63, 20, 0, "NOT_NULL BINARY NUM", "-9223372036854775808"},
      {"-(-9223372036854775808)", "NEWDECIMAL", 63, 20, 0, "NOT_NULL BINARY NUM", "9223372036854775808"},
      {"- -1", "NEWDECIMAL", 63, 2, 0, "NOT_NULL BINARY NUM", "1"},
      {"-0", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "0"},
      {"-0.0", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "0.0"},
      {"-1e0", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "-1"},
  });
}

TEST(ExecExpression, BitOperations) {
  check({
      {"~0", "LONGLONG", 63, 21, 0, "NOT_NULL UNSIGNED BINARY NUM", "18446744073709551615"},
      {"1|2", "LONGLONG", 63, 21, 0, "NOT_NULL UNSIGNED BINARY NUM", "3"},
      {"6&3", "LONGLONG", 63, 21, 0, "NOT_NULL UNSIGNED BINARY NUM", "2"},
      {"1<<3", "LONGLONG", 63, 21, 0, "NOT_NULL UNSIGNED BINARY NUM", "8"},
      {"5^3", "LONGLONG", 63, 21, 0, "NOT_NULL UNSIGNED BINARY NUM", "6"},
      {"~1.5", "LONGLONG", 63, 21, 0, "NOT_NULL UNSIGNED BINARY NUM", "18446744073709551613"},
      {"1.5 | 0", "LONGLONG", 63, 21, 0, "NOT_NULL UNSIGNED BINARY NUM", "2"},
  });
}

TEST(ExecExpression, ComparisonsAndLogic) {
  check({
      {"1=1", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"1<2", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"1<=>NULL", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"1=NULL", "LONGLONG", 63, 1, 0, "BINARY NUM", "NULL"},
      {"NOT 1", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"NOT NULL", "LONGLONG", 63, 1, 0, "BINARY NUM", "NULL"},
      {"1 AND 0", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"1 AND NULL", "LONGLONG", 63, 1, 0, "BINARY NUM", "NULL"},
      {"0 AND NULL", "LONGLONG", 63, 1, 0, "BINARY NUM", "0"},
      {"1 OR NULL", "LONGLONG", 63, 1, 0, "BINARY NUM", "1"},
      {"NULL OR 0", "LONGLONG", 63, 1, 0, "BINARY NUM", "NULL"},
      {"1 XOR 1", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"1 IS NULL", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"NULL IS NULL", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"1 IS TRUE", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"2 IN (1,2)", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"2 BETWEEN 1 AND 3", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
  });
}

TEST(ExecExpression, StringsReadAsNumbers) {
  check({
      {"'1'+1", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "2"},
      {"'1.5x'+0", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "1.5"},
      {"'abc'*2", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "0"},
      // A binary string by its text, unlike a hex literal (observed
      // 2026-11-17).
      {"_binary'12'+1", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "13"},
  });
  Evaluator evaluator;
  evaluator.value(*select("'1.5x'+0").select.items[0].expr);
  ASSERT_EQ(evaluator.warnings().size(), 1u);
  EXPECT_EQ(evaluator.warnings()[0].level, "Warning");
  EXPECT_EQ(evaluator.warnings()[0].code, 1292);
  EXPECT_EQ(evaluator.warnings()[0].message, "Truncated incorrect DOUBLE value: '1.5x'");
  evaluator.clear_warnings();
  evaluator.value(*select("'1'+1").select.items[0].expr);
  EXPECT_TRUE(evaluator.warnings().empty());
}

TEST(ExecExpression, DivisionByZeroIsNullWithAWarning) {
  check({
      {"1/0", "NEWDECIMAL", 63, 7, 4, "BINARY NUM", "NULL"},
      {"1.5/0", "NEWDECIMAL", 63, 8, 5, "BINARY NUM", "NULL"},
      {"1 DIV 0", "LONGLONG", 63, 2, 0, "BINARY NUM", "NULL"},
      {"1 % 0", "LONGLONG", 63, 2, 0, "BINARY NUM", "NULL"},
      {"1/0e0", "DOUBLE", 63, 23, 31, "BINARY NUM", "NULL"},
  });
  Evaluator evaluator;
  evaluator.value(*select("1/0").select.items[0].expr);
  ASSERT_EQ(evaluator.warnings().size(), 1u);
  EXPECT_EQ(evaluator.warnings()[0].code, 1365);
  EXPECT_EQ(evaluator.warnings()[0].message, "Division by 0");
}

void expect_error(const std::string& sql, uint16_t code, const std::string& sqlstate, const std::string& message) {
  SCOPED_TRACE(sql);
  Evaluator evaluator;
  try {
    const auto statement = select(sql);
    evaluator.value(*statement.select.items.at(0).expr);
    FAIL() << "no error";
  } catch (const SqlError& e) {
    EXPECT_EQ(e.code(), code);
    EXPECT_EQ(e.sqlstate(), sqlstate);
    EXPECT_EQ(std::string(e.what()), message);
  }
}

TEST(ExecExpression, OverflowsAreErrorsNamingTheExpression) {
  expect_error("9223372036854775807+1", 1690, "22003", "BIGINT value is out of range in '(9223372036854775807 + 1)'");
  expect_error("-9223372036854775808 - 1", 1690, "22003", "BIGINT value is out of range in '(-(9223372036854775808) - 1)'");
  expect_error("18446744073709551615 + 1", 1690, "22003", "BIGINT UNSIGNED value is out of range in '(18446744073709551615 + 1)'");
  expect_error("0 - 18446744073709551615", 1690, "22003", "BIGINT UNSIGNED value is out of range in '(0 - 18446744073709551615)'");
  expect_error("1e308*10", 1690, "22003", "DOUBLE value is out of range in '(1e308 * 10)'");
}

TEST(ExecExpression, WhatIsNotBuiltIsRefused) {
  Evaluator evaluator;
  const auto refused = [&](const std::string& sql) {
    try {
      evaluator.type(*select(sql).select.items.at(0).expr);
    } catch (const SqlError& e) {
      return e.code();
    }
    return uint16_t{0};
  };
  EXPECT_EQ(refused("'a' REGEXP 'A'"), 1235);
  EXPECT_EQ(refused("SOUNDEX('a')"), 1235);
  EXPECT_EQ(refused("a"), 1054);
  // A character set Kvasir does not yet compare in.
  EXPECT_EQ(refused("_latin1'a' = 'A'"), 1235);
  EXPECT_EQ(refused("N'a' = 'A'"), 1235);
}

// Observed on MySQL 8.4.13, 2026-11-16 (ruling 32): a literal compares under
// collation_connection; a binary string on either side compares bytes.
TEST(ExecExpression, StringsCompareUnderTheConnectionsCollation) {
  check({
      {"'a' = 'A'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"'a' < 'b'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"'B' > 'a'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"'a' != 'á'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"'a' <=> 'A'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"'a' = 'a '", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"'a' IN ('A','b')", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"'b' BETWEEN 'A' AND 'C'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"'ß' = 'ss'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"X'61' = 'a'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"X'41' = 'a'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"'abc' < 'abd'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"NULL = 'a'", "LONGLONG", 63, 1, 0, "BINARY NUM", "NULL"},
      {"'a' IN ('b', NULL)", "LONGLONG", 63, 1, 0, "BINARY NUM", "NULL"},
      {"_utf8mb4'a' = 'A'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"_binary'a' = 'A'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
  });
}

// Observed on MySQL 8.4.13, 2026-11-16.
TEST(ExecExpression, Like) {
  check({
      {"'a' LIKE 'A'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"'abc' NOT LIKE 'a%'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"'a_c' LIKE 'a|_c' ESCAPE '|'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"NULL LIKE 'a'", "LONGLONG", 63, 1, 0, "BINARY NUM", "NULL"},
      {"'a' LIKE NULL", "LONGLONG", 63, 1, 0, "BINARY NUM", "NULL"},
      {"10 LIKE '1%'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"1.5 LIKE '1.%'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"X'61' LIKE 'a'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "1"},
      {"X'61' LIKE 'A'", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
      {"'a%c' LIKE 'a\\\\%c' ESCAPE ''", "LONGLONG", 63, 1, 0, "NOT_NULL BINARY NUM", "0"},
  });
  expect_error("'a' LIKE 'b' ESCAPE 'xy'", 1210, "HY000", "Incorrect arguments to ESCAPE");
}

// COLLATE, observed on MySQL 8.4.13, 2026-11-16: the explicit collation
// wins (coercibility 0); the result is reported in the results character set,
// flagged BINARY for a _bin collation, and never NOT NULL.
TEST(ExecExpression, Collate) {
  check({
      {"'a' COLLATE utf8mb4_bin", "VAR_STRING", 255, 4, 31, "BINARY", "a"},
      {"'a' COLLATE utf8mb4_general_ci", "VAR_STRING", 255, 4, 31, "", "a"},
      {"1 COLLATE utf8mb4_bin", "VAR_STRING", 255, 8, 31, "BINARY", "1"},
      {"'a' = 'A' COLLATE utf8mb4_bin", "LONGLONG", 63, 1, 0, "BINARY NUM", "0"},
      {"'a' COLLATE utf8mb4_bin = 'A'", "LONGLONG", 63, 1, 0, "BINARY NUM", "0"},
      {"'a' COLLATE utf8mb4_0900_as_cs < 'B'", "LONGLONG", 63, 1, 0, "BINARY NUM", "1"},
      {"'ß' = 'ss' COLLATE utf8mb4_general_ci", "LONGLONG", 63, 1, 0, "BINARY NUM", "0"},
      {"'a' COLLATE utf8mb4_BIN = 'A'", "LONGLONG", 63, 1, 0, "BINARY NUM", "0"},
      // An explicit collation outranks a binary string's.
      {"X'41' = 'a' COLLATE utf8mb4_general_ci", "LONGLONG", 63, 1, 0, "BINARY NUM", "1"},
      {"X'61' = 'a' COLLATE utf8mb4_bin", "LONGLONG", 63, 1, 0, "BINARY NUM", "1"},
      {"'a' COLLATE utf8mb4_bin LIKE 'A'", "LONGLONG", 63, 1, 0, "BINARY NUM", "0"},
  });
  const std::string mix = "Illegal mix of collations (utf8mb4_bin,EXPLICIT) and (utf8mb4_general_ci,EXPLICIT) for operation ";
  expect_error("'a' COLLATE utf8mb4_bin = 'A' COLLATE utf8mb4_general_ci", 1267, "HY000", mix + "'='");
  expect_error("'a' COLLATE utf8mb4_bin <> 'A' COLLATE utf8mb4_general_ci", 1267, "HY000", mix + "'<>'");
  expect_error("'a' COLLATE utf8mb4_bin LIKE 'A' COLLATE utf8mb4_general_ci", 1267, "HY000", mix + "'like'");
  const std::string three = "Illegal mix of collations (utf8mb4_bin,EXPLICIT), (utf8mb4_0900_ai_ci,COERCIBLE), (utf8mb4_general_ci,EXPLICIT) for operation ";
  expect_error("'a' COLLATE utf8mb4_bin IN ('A', 'a' COLLATE utf8mb4_general_ci)", 1270, "HY000", three + "' IN '");
  expect_error("'a' COLLATE utf8mb4_bin BETWEEN 'A' AND 'B' COLLATE utf8mb4_general_ci", 1270, "HY000", three + "'between'");
  expect_error("'a' COLLATE latin1_swedish_ci", 1253, "42000", "COLLATION 'latin1_swedish_ci' is not valid for CHARACTER SET 'utf8mb4'");
  expect_error("'a' COLLATE nope", 1273, "HY000", "Unknown collation: 'nope'");
  expect_error("NULL COLLATE utf8mb4_bin", 1253, "42000", "COLLATION 'utf8mb4_bin' is not valid for CHARACTER SET 'binary'");
  expect_error("X'61' COLLATE utf8mb4_bin", 1253, "42000", "COLLATION 'utf8mb4_bin' is not valid for CHARACTER SET 'binary'");
}

TEST(ExecExpression, StringsCompareUnderOtherConnectionCollations) {
  const auto answer = [](const std::string& sql, uint16_t collation) {
    EvalSettings settings;
    settings.result_collation = collation;
    settings.result_max_bytes = collation == 63 ? 1 : 4;
    Evaluator evaluator(settings);
    const auto statement = select(sql);
    return to_text(evaluator.value(*statement.select.items.at(0).expr), evaluator.type(*statement.select.items.at(0).expr)).value_or("NULL");
  };
  EXPECT_EQ(answer("'a' = 'A'", 46), "0");  // utf8mb4_bin
  EXPECT_EQ(answer("'a' = 'a '", 46), "1");
  EXPECT_EQ(answer("'a' = 'A'", 278), "0");  // utf8mb4_0900_as_cs
  EXPECT_EQ(answer("'a' < 'B'", 278), "1");
  EXPECT_EQ(answer("'A' < 'a'", 278), "0");
  EXPECT_EQ(answer("'a' = 'A'", 63), "0");  // binary
  EXPECT_EQ(answer("'a' = 'a '", 63), "0");
}

// The manual's "Flow Control Functions", observed on MySQL 8.4.13,
// 2026-11-17. The result's type is the operands' aggregated (NULL ignored):
// any string makes a string, counted in characters, in the operands'
// collation; else any DOUBLE a DOUBLE; else any DECIMAL a DECIMAL wide
// enough for each; else the widest integer, one wider when signed meets
// unsigned. IFNULL and COALESCE show a DECIMAL at the result's scale, IF and
// CASE at the chosen value's own. Only what is needed is evaluated.
TEST(ExecExpression, FlowControlFunctions) {
  check({
      {"IFNULL(1,2)", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "1"},
      {"IFNULL(NULL,2)", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "2"},
      {"IFNULL(1,NULL)", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "1"},
      {"IFNULL(-12,3)", "LONGLONG", 63, 3, 0, "NOT_NULL BINARY NUM", "-12"},
      {"IFNULL(TRUE,5)", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "1"},
      {"IFNULL(1,2.5)", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "1.0"},
      {"IFNULL(12.345,123.4)", "NEWDECIMAL", 63, 8, 3, "NOT_NULL BINARY NUM", "12.345"},
      {"IFNULL(1.5,2.25)", "NEWDECIMAL", 63, 5, 2, "NOT_NULL BINARY NUM", "1.50"},
      {"IFNULL(18446744073709551615,1)", "NEWDECIMAL", 63, 21, 0, "NOT_NULL BINARY NUM", "18446744073709551615"},
      {"IFNULL(1.5e0,2)", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "1.5"},
      {"IFNULL(2.5,1.5e0)", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "2.5"},
      {"IFNULL(NULL,1.5e0)", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "1.5"},
      {"IFNULL(1,'ab')", "VAR_STRING", 255, 8, 31, "NOT_NULL", "1"},
      {"IFNULL('abc',1)", "VAR_STRING", 255, 12, 31, "NOT_NULL", "abc"},
      {"IFNULL(-12,'a')", "VAR_STRING", 255, 12, 31, "NOT_NULL", "-12"},
      {"IFNULL(2.5,'a')", "VAR_STRING", 255, 16, 31, "NOT_NULL", "2.5"},
      {"IFNULL(1.5e0,'a')", "VAR_STRING", 255, 88, 31, "NOT_NULL", "1.5"},
      {"IFNULL(0x41,'a')", "VAR_STRING", 63, 4, 31, "NOT_NULL BINARY", "A"},
      {"IFNULL(0x41,1)", "VAR_STRING", 63, 2, 31, "NOT_NULL BINARY", "A"},
      {"IFNULL(0x41,0x4243)", "VAR_STRING", 63, 2, 31, "NOT_NULL BINARY", "A"},
      {"IFNULL(_binary'ab','a')", "VAR_STRING", 63, 4, 31, "NOT_NULL BINARY", "ab"},
      {"IFNULL(18446744073709551615,0x01)", "VAR_STRING", 63, 20, 31, "NOT_NULL BINARY", "18446744073709551615"},
      {"IFNULL('a' COLLATE utf8mb4_bin,'b')", "VAR_STRING", 255, 4, 31, "NOT_NULL BINARY", "a"},
      {"IFNULL('a' COLLATE utf8mb4_0900_as_cs,'b')", "VAR_STRING", 255, 4, 31, "NOT_NULL", "a"},
      {"IFNULL(NULL,'a' COLLATE utf8mb4_bin)", "VAR_STRING", 255, 4, 31, "BINARY", "a"},
      {"IFNULL(NULL,NULL)", "NULL", 63, 0, 0, "BINARY NUM", "NULL"},
      {"COALESCE(NULL,1)", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "1"},
      {"COALESCE(1,NULL)", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "1"},
      {"COALESCE(NULL,NULL,'x')", "VAR_STRING", 255, 4, 31, "NOT_NULL", "x"},
      {"COALESCE(1,2.25)", "NEWDECIMAL", 63, 5, 2, "NOT_NULL BINARY NUM", "1.00"},
      {"COALESCE(NULL)", "NULL", 63, 0, 0, "BINARY NUM", "NULL"},
      {"NULLIF(1,1)", "LONGLONG", 63, 2, 0, "BINARY NUM", "NULL"},
      {"NULLIF(1,2)", "LONGLONG", 63, 2, 0, "BINARY NUM", "1"},
      {"NULLIF(1,2.5)", "LONGLONG", 63, 2, 0, "BINARY NUM", "1"},
      {"NULLIF('a','A')", "VAR_STRING", 255, 4, 31, "", "NULL"},
      {"NULLIF(2.5,1)", "NEWDECIMAL", 63, 4, 1, "BINARY NUM", "2.5"},
      {"NULLIF(NULL,1)", "NULL", 63, 0, 0, "BINARY NUM", "NULL"},
      {"IF(1,2,3)", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "2"},
      {"IF(0,'a','bc')", "VAR_STRING", 255, 8, 31, "NOT_NULL", "bc"},
      {"IF(NULL,1,2.5)", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "2.5"},
      {"IF(1,1,2.5)", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "1"},
      {"IF(1,1.5,2.25)", "NEWDECIMAL", 63, 5, 2, "NOT_NULL BINARY NUM", "1.5"},
      {"IF(1,NULL,2)", "LONGLONG", 63, 2, 0, "BINARY NUM", "NULL"},
      {"IF(1,1,1e0)", "DOUBLE", 63, 23, 31, "NOT_NULL BINARY NUM", "1"},
      {"CASE 1 WHEN 1 THEN 'a' ELSE 'bcd' END", "VAR_STRING", 255, 12, 31, "NOT_NULL", "a"},
      {"CASE WHEN 0 THEN 1 END", "LONGLONG", 63, 2, 0, "BINARY NUM", "NULL"},
      {"CASE WHEN 1 THEN 1 ELSE 2.5 END", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "1"},
      {"CASE WHEN 1 THEN 1.0 ELSE 1 END", "NEWDECIMAL", 63, 4, 1, "NOT_NULL BINARY NUM", "1.0"},
      {"CASE WHEN 1 THEN 1 ELSE 'a' END", "VAR_STRING", 255, 8, 31, "NOT_NULL", "1"},
      {"CASE 'a' WHEN 'A' THEN 1 ELSE 0 END", "LONGLONG", 63, 2, 0, "NOT_NULL BINARY NUM", "1"},
      {"CASE 1 WHEN 1.0 THEN 'x' WHEN '1' THEN 'y' END", "VAR_STRING", 255, 4, 31, "", "x"},
      {"CASE 1 WHEN NULL THEN 'x' ELSE 'y' END", "VAR_STRING", 255, 4, 31, "NOT_NULL", "y"},
      {"CASE 'a' COLLATE utf8mb4_bin WHEN 'A' THEN 1 WHEN 'a' THEN 2 END", "LONGLONG", 63, 2, 0, "BINARY NUM", "2"},
      {"CASE 1 WHEN 1 THEN 'a' COLLATE utf8mb4_bin ELSE 'b' END", "VAR_STRING", 255, 4, 31, "BINARY", "a"},
  });
  // A condition or a compared string read as a number warns as it is read.
  for (const std::string sql : {"IF('x',1,2)", "CASE '1a' WHEN 1 THEN 'num' ELSE 'no' END"}) {
    SCOPED_TRACE(sql);
    Evaluator evaluator;
    evaluator.value(*select(sql).select.items[0].expr);
    ASSERT_EQ(evaluator.warnings().size(), 1u);
    EXPECT_EQ(evaluator.warnings()[0].code, 1292);
  }
  {
    Evaluator evaluator;
    EXPECT_EQ(to_text(evaluator.value(*select("CASE '1a' WHEN 1 THEN 'num' ELSE 'no' END").select.items[0].expr), {}).value_or("NULL"), "num");
  }
  // What is not needed is not evaluated: no division, so no warning.
  for (const std::string sql : {"IFNULL(1, 1/0)", "COALESCE(NULL, 1, 1/0)", "IF(1, 2, 1/0)", "CASE WHEN 1 THEN 2 ELSE 1/0 END"}) {
    SCOPED_TRACE(sql);
    Evaluator evaluator;
    evaluator.value(*select(sql).select.items[0].expr);
    EXPECT_TRUE(evaluator.warnings().empty());
  }
  const std::string mix = "Illegal mix of collations (utf8mb4_bin,EXPLICIT) and (utf8mb4_0900_as_cs,EXPLICIT) for operation ";
  expect_error("IFNULL('a' COLLATE utf8mb4_bin, 'b' COLLATE utf8mb4_0900_as_cs)", 1267, "HY000", mix + "'ifnull'");
  expect_error("IF(1, 'a' COLLATE utf8mb4_bin, 'b' COLLATE utf8mb4_0900_as_cs)", 1267, "HY000", mix + "'if'");
  expect_error("NULLIF('a' COLLATE utf8mb4_bin, 'b' COLLATE utf8mb4_0900_as_cs)", 1267, "HY000", mix + "'nullif'");
  expect_error("CASE 1 WHEN 1 THEN 'a' COLLATE utf8mb4_bin ELSE 'b' COLLATE utf8mb4_0900_as_cs END", 1267, "HY000", mix + "'case'");
  expect_error("CASE 'a' COLLATE utf8mb4_bin WHEN 'b' COLLATE utf8mb4_0900_as_cs THEN 1 END", 1267, "HY000", mix + "'case'");
  expect_error("COALESCE('a' COLLATE utf8mb4_bin, 'b' COLLATE utf8mb4_0900_as_cs, 'c')", 1270, "HY000",
               "Illegal mix of collations (utf8mb4_bin,EXPLICIT), (utf8mb4_0900_as_cs,EXPLICIT), (utf8mb4_0900_ai_ci,COERCIBLE) for operation 'coalesce'");
  expect_error("NULLIF(1)", 1582, "42000", "Incorrect parameter count in the call to native function 'NULLIF'");
  expect_error("IFNULL(1)", 1582, "42000", "Incorrect parameter count in the call to native function 'IFNULL'");
  expect_error("IFNULL(1, 2, 3)", 1582, "42000", "Incorrect parameter count in the call to native function 'IFNULL'");
}

// The manual's "String Functions and Operators": CONCAT, CONCAT_WS, LENGTH
// (OCTET_LENGTH) and CHAR_LENGTH (CHARACTER_LENGTH), observed on MySQL
// 8.4.13, 2026-11-17. CONCAT is NULL when any argument is, CONCAT_WS skips
// NULL arguments (NULL only for a NULL separator); both are as long as
// their arguments' text together, in the arguments' aggregated collation (a
// number's at NUMERIC coercibility, NULL's IGNORABLE), and never NOT NULL.
// LENGTH counts bytes, CHAR_LENGTH characters (bytes of a binary string).
TEST(ExecExpression, ConcatAndLength) {
  check({
      {"CONCAT('a','bc')", "VAR_STRING", 255, 12, 31, "", "abc"},
      {"CONCAT('a',1)", "VAR_STRING", 255, 12, 31, "", "a1"},
      {"CONCAT('a',NULL)", "VAR_STRING", 255, 4, 31, "", "NULL"},
      {"CONCAT(1,2.5)", "VAR_STRING", 255, 24, 31, "", "12.5"},
      {"CONCAT(0x41,'b')", "VAR_STRING", 63, 5, 31, "BINARY", "Ab"},
      {"CONCAT('a' COLLATE utf8mb4_bin,'b')", "VAR_STRING", 255, 8, 31, "BINARY", "ab"},
      {"CONCAT(1.5e0)", "VAR_STRING", 255, 88, 31, "", "1.5"},
      {"CONCAT(NULL)", "VAR_STRING", 63, 0, 31, "BINARY", "NULL"},
      {"CONCAT(-12)", "VAR_STRING", 255, 12, 31, "", "-12"},
      {"CONCAT(_binary'a','\xC3\xA9')", "VAR_STRING", 63, 5, 31, "BINARY", "a\xC3\xA9"},
      {"CONCAT_WS(',','a','b')", "VAR_STRING", 255, 12, 31, "", "a,b"},
      {"CONCAT_WS(',','a',NULL,'b')", "VAR_STRING", 255, 16, 31, "", "a,b"},
      {"CONCAT_WS(NULL,'a')", "VAR_STRING", 255, 4, 31, "", "NULL"},
      {"CONCAT_WS(',',1,2)", "VAR_STRING", 255, 20, 31, "", "1,2"},
      {"CONCAT_WS('--','a')", "VAR_STRING", 255, 4, 31, "", "a"},
      {"LENGTH('abc')", "LONGLONG", 63, 10, 0, "NOT_NULL BINARY NUM", "3"},
      {"LENGTH('\xC3\xA9')", "LONGLONG", 63, 10, 0, "NOT_NULL BINARY NUM", "2"},
      {"LENGTH(NULL)", "LONGLONG", 63, 10, 0, "BINARY NUM", "NULL"},
      {"LENGTH(123)", "LONGLONG", 63, 10, 0, "NOT_NULL BINARY NUM", "3"},
      {"LENGTH(1.5e0)", "LONGLONG", 63, 10, 0, "NOT_NULL BINARY NUM", "3"},
      {"OCTET_LENGTH('\xC3\xA9')", "LONGLONG", 63, 10, 0, "NOT_NULL BINARY NUM", "2"},
      {"CHAR_LENGTH('\xC3\xA9')", "LONGLONG", 63, 10, 0, "NOT_NULL BINARY NUM", "1"},
      {"CHARACTER_LENGTH('abc')", "LONGLONG", 63, 10, 0, "NOT_NULL BINARY NUM", "3"},
      {"CHAR_LENGTH(0x4142)", "LONGLONG", 63, 10, 0, "NOT_NULL BINARY NUM", "2"},
  });
  expect_error("CONCAT('a' COLLATE utf8mb4_bin, 'b' COLLATE utf8mb4_0900_as_cs)", 1267, "HY000",
               "Illegal mix of collations (utf8mb4_bin,EXPLICIT) and (utf8mb4_0900_as_cs,EXPLICIT) for operation 'concat'");
  expect_error("CONCAT()", 1582, "42000", "Incorrect parameter count in the call to native function 'CONCAT'");
  expect_error("CONCAT_WS(',')", 1582, "42000", "Incorrect parameter count in the call to native function 'CONCAT_WS'");
  expect_error("LENGTH()", 1582, "42000", "Incorrect parameter count in the call to native function 'LENGTH'");
  expect_error("LENGTH(1,2)", 1582, "42000", "Incorrect parameter count in the call to native function 'LENGTH'");
}

// SUBSTRING (SUBSTR, MID), LEFT and RIGHT, observed on MySQL 8.4.13,
// 2026-11-17: in characters (bytes of a binary string), a position from 1,
// or from the end when negative, 0 giving nothing; a number rounded to an
// integer. Never NOT NULL; as long as the string, less what a constant
// position or length takes away.
TEST(ExecExpression, SubstringLeftAndRight) {
  check({
      {"SUBSTRING('abcdef',2)", "VAR_STRING", 255, 20, 31, "", "bcdef"},
      {"SUBSTRING('abcdef',2,3)", "VAR_STRING", 255, 12, 31, "", "bcd"},
      {"SUBSTRING('abcdef',-2)", "VAR_STRING", 255, 8, 31, "", "ef"},
      {"SUBSTRING('abcdef',0)", "VAR_STRING", 255, 0, 31, "", ""},
      {"SUBSTRING('abcdef',2,-1)", "VAR_STRING", 255, 0, 31, "", ""},
      {"SUBSTRING('abcdef',-10)", "VAR_STRING", 255, 0, 31, "", ""},
      {"SUBSTRING('abcdef',7)", "VAR_STRING", 255, 0, 31, "", ""},
      {"SUBSTRING('abcdef',3,100)", "VAR_STRING", 255, 16, 31, "", "cdef"},
      {"SUBSTRING('abcdef',2.5)", "VAR_STRING", 255, 16, 31, "", "cdef"},
      {"SUBSTRING('abcdef',1.5e0)", "VAR_STRING", 255, 20, 31, "", "bcdef"},
      {"SUBSTR('h\xC3\xA9llo',2,2)", "VAR_STRING", 255, 8, 31, "", "\xC3\xA9l"},
      {"MID('abc',2,1)", "VAR_STRING", 255, 4, 31, "", "b"},
      {"MID('a',1)", "VAR_STRING", 255, 4, 31, "", "a"},
      {"SUBSTRING('abcdef' FROM 3)", "VAR_STRING", 255, 16, 31, "", "cdef"},
      {"SUBSTRING('abcdef' FROM 2 FOR 2)", "VAR_STRING", 255, 8, 31, "", "bc"},
      {"SUBSTR('abc' FROM 2)", "VAR_STRING", 255, 8, 31, "", "bc"},
      {"SUBSTRING(12345,2,2)", "VAR_STRING", 255, 8, 31, "", "23"},
      {"SUBSTRING(NULL,1)", "VAR_STRING", 63, 0, 31, "BINARY", "NULL"},
      {"SUBSTRING('abc',NULL)", "VAR_STRING", 255, 12, 31, "", "NULL"},
      {"SUBSTRING('abc',1,NULL)", "VAR_STRING", 255, 12, 31, "", "NULL"},
      {"SUBSTRING(_binary'h\xC3\xA9llo',2,2)", "VAR_STRING", 63, 2, 31, "BINARY", "\xC3\xA9"},
      {"LEFT('abcdef',2)", "VAR_STRING", 255, 8, 31, "", "ab"},
      {"RIGHT('abcdef',2)", "VAR_STRING", 255, 8, 31, "", "ef"},
      {"LEFT('abc',-1)", "VAR_STRING", 255, 0, 31, "", ""},
      {"RIGHT('abc',-1)", "VAR_STRING", 255, 0, 31, "", ""},
      {"LEFT('abc',10)", "VAR_STRING", 255, 12, 31, "", "abc"},
      {"LEFT(NULL,1)", "VAR_STRING", 63, 0, 31, "BINARY", "NULL"},
      {"LEFT('abc',NULL)", "VAR_STRING", 255, 12, 31, "", "NULL"},
      {"LEFT(12345,2)", "VAR_STRING", 255, 8, 31, "", "12"},
      {"RIGHT('h\xC3\xA9llo',3)", "VAR_STRING", 255, 12, 31, "", "llo"},
      {"LEFT('abc',1.5)", "VAR_STRING", 255, 8, 31, "", "ab"},
      {"LEFT('abc','2x')", "VAR_STRING", 255, 8, 31, "", "ab"},
      {"RIGHT(_binary'h\xC3\xA9llo',2)", "VAR_STRING", 63, 2, 31, "BINARY", "lo"},
      {"LEFT(_binary'ab',1)", "VAR_STRING", 63, 1, 31, "BINARY", "a"},
  });
  // A string read as an integer warns of what is left over.
  Evaluator evaluator;
  evaluator.value(*select("LEFT('abc','2x')").select.items[0].expr);
  ASSERT_FALSE(evaluator.warnings().empty());
  EXPECT_EQ(evaluator.warnings().back().code, 1292);
  EXPECT_EQ(evaluator.warnings().back().message, "Truncated incorrect INTEGER value: '2x'");
}

TEST(ExecExpression, ColumnNames) {
  const auto names = [](const std::string& sql) {
    std::vector<std::string> out;
    for (const auto& item : select(sql).select.items) out.push_back(column_name(item));
    return out;
  };
  EXPECT_EQ(names("1+1, 'abc', _latin1'x', 1 AS one, 'a' 'b', 0x41, @@version_comment"),
            (std::vector<std::string>{"1+1", "abc", "x", "one", "a", "0x41", "@@version_comment"}));
  // Names are held in utf8mb3: a character past the BMP becomes '?'
  // (observed).
  EXPECT_EQ(names("'\xF0\x9F\x98\x80', 'a\xF0\x9F\x98\x80"
                  "b', '\xF0\x9F\x98\x80' LIKE '_'"),
            (std::vector<std::string>{"?", "a?b", "'?' LIKE '_'"}));
}

}  // namespace
}  // namespace kvasir::exec
