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
  EXPECT_EQ(refused("'a' LIKE 'A'"), 1235);
  EXPECT_EQ(refused("CONCAT('a', 'b')"), 1235);
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

TEST(ExecExpression, ColumnNames) {
  const auto names = [](const std::string& sql) {
    std::vector<std::string> out;
    for (const auto& item : select(sql).select.items) out.push_back(column_name(item));
    return out;
  };
  EXPECT_EQ(names("1+1, 'abc', _latin1'x', 1 AS one, 'a' 'b', 0x41, @@version_comment"),
            (std::vector<std::string>{"1+1", "abc", "x", "one", "a", "0x41", "@@version_comment"}));
}

}  // namespace
}  // namespace kvasir::exec
