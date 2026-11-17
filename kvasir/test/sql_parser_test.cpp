//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual:
// - "Operator Precedence", highest first: INTERVAL; BINARY, COLLATE; !;
//   - (unary), ~; ^; * / DIV % MOD; - +; << >>; &; |; = <=> >= > <= < <> !=
//   IS LIKE REGEXP IN MEMBER OF; BETWEEN CASE WHEN THEN ELSE; NOT; AND &&;
//   XOR; OR ||; = := (assignment). Operators of one level group left.
// - "Keywords and Reserved Words": a reserved word is a name only quoted.
// - "String Literals": adjacent strings are joined; _charset'..' and N'..'.
// - SELECT, SET (variables, NAMES, CHARACTER SET / CHARSET), USE, START
//   TRANSACTION / BEGIN, COMMIT, ROLLBACK, with their optional WORK.
// - "Query was empty" (1065) for nothing to run.
// - MySQL names an unaliased select item by its text as written.

#include <gtest/gtest.h>

#include <string>

#include "kvasir/sql/keywords.h"
#include "kvasir/sql/parser.h"

namespace kvasir::sql {
namespace {

using Kind = Statement::Kind;
using Target = Assignment::Target;

Statement one(std::string_view text, const ParserOptions& options = {}) {
  auto statements = parse(text, options);
  EXPECT_EQ(statements.size(), 1u) << text;
  return std::move(statements.at(0));
}

// The first select item of SELECT <text>, printed.
std::string expr(const std::string& text, const ParserOptions& options = {}) {
  const auto statement = one("SELECT " + text, options);
  EXPECT_EQ(statement.select.items.size(), 1u) << text;
  return to_string(*statement.select.items.at(0).expr);
}

size_t error_at(std::string_view text, const ParserOptions& options = {}) {
  try {
    parse(text, options);
  } catch (const SyntaxError& e) {
    return e.offset();
  }
  ADD_FAILURE() << "parsed: " << text;
  return SIZE_MAX;
}

TEST(SqlKeywords, ReservedWordsAreTheManuals) {
  for (const char* word : {"SELECT", "select", "Qualify", "TABLESAMPLE", "READ_WRITE", "SQL_CALC_FOUND_ROWS", "ZEROFILL", "ACCESSIBLE", "INT1", "DUAL"})
    EXPECT_TRUE(is_reserved(word)) << word;
  for (const char* word : {"", "autocommit", "NAMES", "WORK", "BEGIN", "COMMIT", "ROLLBACK", "START", "TRANSACTION", "GLOBAL", "SESSION", "OFFSET", "COUNT",
                           "MASTER_BIND", "SELECTS", "REA", "ZEROFILLS"})
    EXPECT_FALSE(is_reserved(word)) << word;
}

TEST(SqlParser, ArithmeticPrecedence) {
  EXPECT_EQ(expr("1 + 2 * 3"), "(+ 1 (* 2 3))");
  EXPECT_EQ(expr("1 - 2 - 3"), "(- (- 1 2) 3)");
  EXPECT_EQ(expr("(1 + 2) * 3"), "(* (+ 1 2) 3)");
  EXPECT_EQ(expr("6 DIV 4 MOD 3 % 2 / 1"), "(/ (% (% (DIV 6 4) 3) 2) 1)");
  EXPECT_EQ(expr("-2 ^ 3"), "(^ (- 2) 3)");
  EXPECT_EQ(expr("2 ^ 3 * 4"), "(* (^ 2 3) 4)");
  EXPECT_EQ(expr("- - 1"), "(- (- 1))");
  EXPECT_EQ(expr("+1"), "1");
  EXPECT_EQ(expr("~1"), "(~ 1)");
}

TEST(SqlParser, BitPrecedence) {
  EXPECT_EQ(expr("1 << 2 & 3 | 4"), "(| (& (<< 1 2) 3) 4)");
  EXPECT_EQ(expr("1 + 2 << 3"), "(<< (+ 1 2) 3)");
}

TEST(SqlParser, LogicalPrecedence) {
  EXPECT_EQ(expr("a OR b AND c"), "(OR a (AND b c))");
  EXPECT_EQ(expr("a XOR b OR c"), "(OR (XOR a b) c)");
  EXPECT_EQ(expr("a AND b XOR c"), "(XOR (AND a b) c)");
  EXPECT_EQ(expr("NOT a = b"), "(NOT (= a b))");
  EXPECT_EQ(expr("! a = b"), "(= (NOT a) b)");
  EXPECT_EQ(expr("NOT NOT a"), "(NOT (NOT a))");
  EXPECT_EQ(expr("a && b || c"), "(OR (AND a b) c)");
}

TEST(SqlParser, PipesAsConcat) {
  ParserOptions options;
  options.pipes_as_concat = true;
  EXPECT_EQ(expr("a || b || c", options), "(CONCAT (CONCAT a b) c)");
}

TEST(SqlParser, Comparisons) {
  EXPECT_EQ(expr("a = b = c"), "(= (= a b) c)");
  EXPECT_EQ(expr("a <> b"), "(!= a b)");
  EXPECT_EQ(expr("a != b"), "(!= a b)");
  EXPECT_EQ(expr("a <=> b"), "(<=> a b)");
  EXPECT_EQ(expr("a < b + 1"), "(< a (+ b 1))");
  EXPECT_EQ(expr("a >= b AND c <= d"), "(AND (>= a b) (<= c d))");
}

TEST(SqlParser, IsTests) {
  EXPECT_EQ(expr("a IS NULL"), "(IS a NULL)");
  EXPECT_EQ(expr("a IS NOT NULL"), "(IS_NOT a NULL)");
  EXPECT_EQ(expr("a = 1 IS TRUE"), "(IS (= a 1) TRUE)");
  EXPECT_EQ(expr("a IS NOT UNKNOWN"), "(IS_NOT a UNKNOWN)");
  EXPECT_EQ(expr("a IS FALSE"), "(IS a FALSE)");
}

TEST(SqlParser, Predicates) {
  EXPECT_EQ(expr("a IN (1, 2)"), "(IN a 1 2)");
  EXPECT_EQ(expr("a NOT IN (1)"), "(NOT_IN a 1)");
  EXPECT_EQ(expr("a BETWEEN 1 AND 2 AND b"), "(AND (BETWEEN a 1 2) b)");
  EXPECT_EQ(expr("a NOT BETWEEN 1 + 1 AND 2"), "(NOT_BETWEEN a (+ 1 1) 2)");
  EXPECT_EQ(expr("a LIKE 'x%'"), "(LIKE a 'x%')");
  EXPECT_EQ(expr("a NOT LIKE 'x!%' ESCAPE '!'"), "(NOT_LIKE a 'x!%' '!')");
  EXPECT_EQ(expr("a REGEXP 'x'"), "(REGEXP a 'x')");
  EXPECT_EQ(expr("a NOT RLIKE 'x'"), "(NOT_REGEXP a 'x')");
}

TEST(SqlParser, Literals) {
  EXPECT_EQ(expr("NULL"), "NULL");
  EXPECT_EQ(expr("true"), "TRUE");
  EXPECT_EQ(expr("False"), "FALSE");
  EXPECT_EQ(expr("1.5e3"), "1.5e3");
  EXPECT_EQ(expr(".5"), ".5");
  EXPECT_EQ(expr("0x41"), "X'41'");
  EXPECT_EQ(expr("b'1'"), "X'01'");
  EXPECT_EQ(expr("'it''s'"), "'it''s'");
  EXPECT_EQ(expr("?"), "?");
}

TEST(SqlParser, AdjacentStringsAreJoined) {
  EXPECT_EQ(expr("'a' 'b' \"c\""), "'abc'");
  EXPECT_EQ(expr("_utf8mb4'x' 'y'"), "_utf8mb4'xy'");
  EXPECT_EQ(expr("N'x'"), "N'x'");
}

TEST(SqlParser, Names) {
  EXPECT_EQ(expr("t.c"), "t.c");
  EXPECT_EQ(expr("db.t.c"), "db.t.c");
  EXPECT_EQ(expr("`t`.`c`"), "t.c");
  EXPECT_EQ(expr("`select`"), "select");
  EXPECT_EQ(expr("t.select"), "t.select");
}

TEST(SqlParser, Variables) {
  EXPECT_EQ(expr("@@version_comment"), "@@version_comment");
  EXPECT_EQ(expr("@@SESSION.autocommit"), "@@SESSION.autocommit");
  EXPECT_EQ(expr("@@global.max_connections"), "@@GLOBAL.max_connections");
  EXPECT_EQ(expr("@@local.sql_mode"), "@@LOCAL.sql_mode");
  EXPECT_EQ(expr("@a"), "@a");
  EXPECT_EQ(expr("@a := 1"), "(:= @a 1)");
  EXPECT_EQ(expr("@a := @b := 2"), "(:= @a (:= @b 2))");
}

TEST(SqlParser, Calls) {
  EXPECT_EQ(expr("concat('a', b)"), "(CONCAT 'a' b)");
  EXPECT_EQ(expr("NOW()"), "(NOW)");
  EXPECT_EQ(expr("count(*)"), "(COUNT *)");
  EXPECT_EQ(expr("COUNT(DISTINCT a, b)"), "(COUNT DISTINCT a b)");
  EXPECT_EQ(expr("IF(a, 1, 2)"), "(IF a 1 2)");
  EXPECT_EQ(expr("LEFT('abc', 1)"), "(LEFT 'abc' 1)");
  EXPECT_EQ(expr("CURRENT_TIMESTAMP"), "(CURRENT_TIMESTAMP)");
  EXPECT_EQ(expr("current_user()"), "(CURRENT_USER)");
  EXPECT_EQ(expr("DATABASE()"), "(DATABASE)");
  EXPECT_EQ(expr("ROW(1, 2)"), "(ROW 1 2)");
}

TEST(SqlParser, RowsAndParentheses) {
  EXPECT_EQ(expr("(1)"), "1");
  EXPECT_EQ(expr("((1))"), "1");
  EXPECT_EQ(expr("(1, 2)"), "(ROW 1 2)");
  EXPECT_EQ(expr("(a, b) = (1, 2)"), "(= (ROW a b) (ROW 1 2))");
}

TEST(SqlParser, Case) {
  EXPECT_EQ(expr("CASE a WHEN 1 THEN 'x' WHEN 2 THEN 'y' ELSE 'z' END"), "(CASE a (WHEN 1 'x') (WHEN 2 'y') (ELSE 'z'))");
  EXPECT_EQ(expr("CASE WHEN a > 1 THEN b END"), "(CASE (WHEN (> a 1) b))");
}

TEST(SqlParser, CollateAndBinary) {
  EXPECT_EQ(expr("a COLLATE utf8mb4_bin"), "(COLLATE a utf8mb4_bin)");
  EXPECT_EQ(expr("a COLLATE 'utf8mb4_bin' = b"), "(= (COLLATE a utf8mb4_bin) b)");
  EXPECT_EQ(expr("BINARY a = b"), "(= (BINARY a) b)");
}

TEST(SqlParser, NestingIsBounded) {
  ParserOptions options;
  options.max_depth = 50;
  EXPECT_EQ(expr(std::string(20, '(') + "1" + std::string(20, ')'), options), "1");
  EXPECT_NE(error_at("SELECT " + std::string(100, '(') + "1" + std::string(100, ')'), options), SIZE_MAX);
  EXPECT_NE(error_at("SELECT " + std::string(100000, '-') + "1"), SIZE_MAX);
  EXPECT_NE(error_at("SELECT " + std::string(100000, '(')), SIZE_MAX);
}

TEST(SqlParser, TheVersionCommentQuery) {
  const auto s = one("select @@version_comment limit 1");
  EXPECT_EQ(s.kind, Kind::select);
  ASSERT_EQ(s.select.items.size(), 1u);
  EXPECT_EQ(s.select.items[0].text, "@@version_comment");
  EXPECT_FALSE(s.select.items[0].has_alias);
  ASSERT_TRUE(s.select.limit);
  EXPECT_EQ(to_string(*s.select.limit), "1");
  EXPECT_FALSE(s.select.offset);
}

TEST(SqlParser, SelectItemsAndAliases) {
  const auto s = one("SELECT 1 AS one, 2 two, 3 'three', 4 AS \"four\", 1 + 1, `x` AS `y`");
  ASSERT_EQ(s.select.items.size(), 6u);
  EXPECT_EQ(s.select.items[0].alias, "one");
  EXPECT_EQ(s.select.items[1].alias, "two");
  EXPECT_EQ(s.select.items[2].alias, "three");
  EXPECT_EQ(s.select.items[3].alias, "four");
  EXPECT_FALSE(s.select.items[4].has_alias);
  EXPECT_EQ(s.select.items[4].text, "1 + 1");
  EXPECT_EQ(s.select.items[5].alias, "y");
}

TEST(SqlParser, SelectFromWhereOrderLimit) {
  const auto s = one("SELECT t.*, a FROM db.t AS x, u WHERE a = 1 ORDER BY a DESC, b ASC, c LIMIT 10 OFFSET 5");
  ASSERT_EQ(s.select.items.size(), 2u);
  EXPECT_TRUE(s.select.items[0].star);
  EXPECT_EQ(s.select.items[0].star_path, (std::vector<std::string>{"t"}));
  ASSERT_EQ(s.select.from.size(), 2u);
  EXPECT_EQ(s.select.from[0].name, (std::vector<std::string>{"db", "t"}));
  EXPECT_EQ(s.select.from[0].alias, "x");
  EXPECT_EQ(s.select.from[1].name, (std::vector<std::string>{"u"}));
  EXPECT_EQ(to_string(*s.select.where), "(= a 1)");
  ASSERT_EQ(s.select.order.size(), 3u);
  EXPECT_TRUE(s.select.order[0].descending);
  EXPECT_FALSE(s.select.order[1].descending);
  EXPECT_FALSE(s.select.order[2].descending);
  EXPECT_EQ(to_string(*s.select.limit), "10");
  EXPECT_EQ(to_string(*s.select.offset), "5");
}

TEST(SqlParser, LimitWithACommaPutsTheOffsetFirst) {
  const auto s = one("SELECT * FROM t LIMIT 5, 10");
  EXPECT_TRUE(s.select.items[0].star);
  EXPECT_TRUE(s.select.items[0].star_path.empty());
  EXPECT_EQ(to_string(*s.select.offset), "5");
  EXPECT_EQ(to_string(*s.select.limit), "10");
}

TEST(SqlParser, SelectFromDualAndDistinct) {
  EXPECT_TRUE(one("SELECT 1 FROM DUAL").select.from_dual);
  EXPECT_TRUE(one("SELECT DISTINCT a FROM t").select.distinct);
  EXPECT_FALSE(one("SELECT ALL a FROM t").select.distinct);
  EXPECT_TRUE(one("SELECT DISTINCTROW a FROM t").select.distinct);
}

// The manual's SELECT: [GROUP BY {col_name | expr | position}, ... [WITH
// ROLLUP]] [HAVING where_condition], between WHERE and ORDER BY; GROUP BY
// takes no ASC or DESC since 8.0.
TEST(SqlParser, GroupByAndHaving) {
  auto s = one("SELECT g, COUNT(*) FROM t WHERE a > 1 GROUP BY g, 2, a + 1 HAVING COUNT(*) > 1 ORDER BY g LIMIT 3");
  ASSERT_EQ(s.select.group_by.size(), 3u);
  EXPECT_EQ(to_string(*s.select.group_by[0]), "g");
  EXPECT_EQ(to_string(*s.select.group_by[1]), "2");
  EXPECT_EQ(to_string(*s.select.group_by[2]), "(+ a 1)");
  EXPECT_FALSE(s.select.with_rollup);
  EXPECT_EQ(to_string(*s.select.having), "(> (COUNT *) 1)");
  EXPECT_EQ(to_string(*s.select.where), "(> a 1)");
  EXPECT_EQ(s.select.order.size(), 1u);
  EXPECT_TRUE(one("SELECT g FROM t GROUP BY g WITH ROLLUP").select.with_rollup);
  s = one("SELECT COUNT(*) FROM t HAVING COUNT(*) > 0");
  EXPECT_TRUE(s.select.group_by.empty());
  EXPECT_EQ(to_string(*s.select.having), "(> (COUNT *) 0)");
  EXPECT_EQ(error_at("SELECT g FROM t GROUP BY g DESC"), 27u);
  EXPECT_EQ(error_at("SELECT g FROM t GROUP g"), 22u);
}

// The grammar's own functions (observed on MySQL 8.4.13, 2026-11-17): IF
// takes exactly three arguments and COALESCE at least one, or the statement
// is not SQL (1064, near the token where it went wrong).
TEST(SqlParser, IfAndCoalesceArguments) {
  EXPECT_EQ(to_string(*one("SELECT IF(a, 1, 2)").select.items[0].expr), "(IF a 1 2)");
  EXPECT_EQ(to_string(*one("SELECT COALESCE(a)").select.items[0].expr), "(COALESCE a)");
  EXPECT_EQ(error_at("SELECT COALESCE()"), 16u);
  EXPECT_EQ(error_at("SELECT IF(1,2)"), 13u);
  EXPECT_EQ(error_at("SELECT IF()"), 10u);
  EXPECT_EQ(error_at("SELECT IF(1,2,3,4)"), 15u);
}

// SUBSTRING (SUBSTR, MID) takes (str, pos[, len]) or (str FROM pos [FOR
// len]), LEFT and RIGHT (str, len); anything else is not SQL (observed on
// MySQL 8.4.13, 2026-11-17).
TEST(SqlParser, SubstringLeftAndRightArguments) {
  EXPECT_EQ(to_string(*one("SELECT SUBSTRING('abc' FROM 2 FOR 1)").select.items[0].expr), "(SUBSTRING 'abc' 2 1)");
  EXPECT_EQ(to_string(*one("SELECT SUBSTR(a FROM 2)").select.items[0].expr), "(SUBSTR a 2)");
  EXPECT_EQ(to_string(*one("SELECT MID(a, 2, 3)").select.items[0].expr), "(MID a 2 3)");
  EXPECT_EQ(to_string(*one("SELECT LEFT(a, 2)").select.items[0].expr), "(LEFT a 2)");
  EXPECT_EQ(error_at("SELECT SUBSTRING('a')"), 20u);
  EXPECT_EQ(error_at("SELECT SUBSTRING('a',1,2,3)"), 24u);
  EXPECT_EQ(error_at("SELECT MID('a')"), 14u);
  EXPECT_EQ(error_at("SELECT LEFT('a')"), 15u);
  EXPECT_EQ(error_at("SELECT RIGHT('a',1,2)"), 18u);
}

// TRIM([{BOTH | LEADING | TRAILING} [remstr] FROM] str), TRIM([remstr
// FROM] str), POSITION(substr IN str), REPLACE's three arguments and
// REPEAT's two, from the grammar (observed on MySQL 8.4.13, 2026-11-17).
TEST(SqlParser, TrimPositionReplaceAndRepeatArguments) {
  const auto text = [](const std::string& sql) { return to_string(*one(sql).select.items[0].expr); };
  EXPECT_EQ(text("SELECT TRIM(a)"), "(TRIM a)");
  EXPECT_EQ(text("SELECT TRIM('x' FROM a)"), "(TRIM a 'x')");
  EXPECT_EQ(text("SELECT TRIM(LEADING 'x' FROM a)"), "(TRIM LEADING a 'x')");
  EXPECT_EQ(text("SELECT TRIM(TRAILING FROM a)"), "(TRIM TRAILING a)");
  EXPECT_EQ(text("SELECT TRIM(BOTH 'x' FROM a)"), "(TRIM BOTH a 'x')");
  EXPECT_EQ(text("SELECT POSITION('c' IN 'abc')"), "(POSITION 'c' 'abc')");
  EXPECT_EQ(text("SELECT REPLACE(a, 'b', 'c')"), "(REPLACE a 'b' 'c')");
  EXPECT_EQ(error_at("SELECT REPLACE('a','b')"), 22u);
  EXPECT_EQ(error_at("SELECT REPEAT('a')"), 17u);
  EXPECT_EQ(error_at("SELECT REVERSE()"), 15u);
  EXPECT_EQ(error_at("SELECT TRIM()"), 12u);
  EXPECT_EQ(error_at("SELECT TRIM(LEADING 'a' 'b')"), 27u);
  EXPECT_EQ(error_at("SELECT POSITION('a')"), 19u);
}

// MOD(a, b) is the % operator, with exactly two arguments (MySQL's grammar).
TEST(SqlParser, ModFunction) {
  EXPECT_EQ(to_string(*one("SELECT MOD(a, 3)").select.items[0].expr), "(% a 3)");
  EXPECT_EQ(error_at("SELECT MOD(1)"), 12u);
}

TEST(SqlParser, SelectHint) { EXPECT_EQ(one("SELECT /*+ MAX_EXECUTION_TIME(1000) */ 1").select.hint, " MAX_EXECUTION_TIME(1000) "); }

TEST(SqlParser, SetVariables) {
  const auto s = one("SET autocommit=1, @a = 2, GLOBAL max_connections = 10, @@session.sql_mode = 'ANSI', SESSION x = DEFAULT, y = ON, z := 3");
  EXPECT_EQ(s.kind, Kind::set);
  ASSERT_EQ(s.assignments.size(), 7u);
  EXPECT_EQ(s.assignments[0].target, Target::system_variable);
  EXPECT_EQ(s.assignments[0].scope, "");
  EXPECT_EQ(s.assignments[0].name, "autocommit");
  EXPECT_EQ(to_string(*s.assignments[0].value), "1");
  EXPECT_EQ(s.assignments[1].target, Target::user_variable);
  EXPECT_EQ(s.assignments[1].name, "a");
  EXPECT_EQ(s.assignments[2].scope, "GLOBAL");
  EXPECT_EQ(s.assignments[2].name, "max_connections");
  EXPECT_EQ(s.assignments[3].scope, "SESSION");
  EXPECT_EQ(s.assignments[3].name, "sql_mode");
  EXPECT_EQ(to_string(*s.assignments[3].value), "'ANSI'");
  EXPECT_EQ(s.assignments[4].scope, "SESSION");
  EXPECT_EQ(to_string(*s.assignments[4].value), "DEFAULT");
  EXPECT_EQ(to_string(*s.assignments[5].value), "ON");
  EXPECT_EQ(s.assignments[6].name, "z");
}

TEST(SqlParser, SetScopes) {
  EXPECT_EQ(one("SET @@local.a = 1").assignments[0].scope, "LOCAL");
  EXPECT_EQ(one("SET PERSIST a = 1").assignments[0].scope, "PERSIST");
  EXPECT_EQ(one("SET PERSIST_ONLY a = 1").assignments[0].scope, "PERSIST_ONLY");
  EXPECT_EQ(one("SET @@a = 1").assignments[0].scope, "");
  EXPECT_EQ(one("SET LOCAL a = 1").assignments[0].scope, "LOCAL");
}

TEST(SqlParser, SetNamesAndCharacterSet) {
  auto s = one("SET NAMES utf8mb4");
  ASSERT_EQ(s.assignments.size(), 1u);
  EXPECT_EQ(s.assignments[0].target, Target::names);
  EXPECT_EQ(s.assignments[0].charset, "utf8mb4");
  s = one("SET NAMES 'utf8mb4' COLLATE 'utf8mb4_0900_ai_ci'");
  EXPECT_EQ(s.assignments[0].charset, "utf8mb4");
  EXPECT_EQ(s.assignments[0].collation, "utf8mb4_0900_ai_ci");
  EXPECT_TRUE(one("SET NAMES DEFAULT").assignments[0].use_default);
  s = one("SET CHARACTER SET utf8mb4");
  EXPECT_EQ(s.assignments[0].target, Target::character_set);
  EXPECT_EQ(s.assignments[0].charset, "utf8mb4");
  EXPECT_EQ(one("SET CHARSET latin1").assignments[0].charset, "latin1");
  // BINARY, a reserved word, names the binary character set (the manual's
  // SET NAMES, and observed).
  EXPECT_EQ(one("SET NAMES binary").assignments[0].charset, "binary");
  EXPECT_EQ(one("SET CHARACTER SET BINARY").assignments[0].charset, "binary");
  EXPECT_TRUE(one("SET CHARSET DEFAULT").assignments[0].use_default);
  s = one("SET NAMES utf8mb4, autocommit = 0");
  ASSERT_EQ(s.assignments.size(), 2u);
  EXPECT_EQ(s.assignments[1].name, "autocommit");
}

TEST(SqlParser, Use) {
  EXPECT_EQ(one("USE shop").database, "shop");
  EXPECT_EQ(one("use `my db`").database, "my db");
  EXPECT_EQ(one("USE shop").kind, Kind::use);
}

TEST(SqlParser, Transactions) {
  EXPECT_EQ(one("BEGIN").kind, Kind::begin);
  EXPECT_EQ(one("BEGIN WORK").kind, Kind::begin);
  EXPECT_EQ(one("START TRANSACTION").kind, Kind::begin);
  EXPECT_TRUE(one("START TRANSACTION READ ONLY").read_only);
  EXPECT_FALSE(one("START TRANSACTION READ WRITE").read_only);
  EXPECT_EQ(one("start transaction with consistent snapshot").kind, Kind::begin);
  EXPECT_EQ(one("COMMIT").kind, Kind::commit);
  EXPECT_EQ(one("COMMIT WORK").kind, Kind::commit);
  EXPECT_EQ(one("COMMIT /*+ SET_VAR(ack_level='ephemeral') */").hint, " SET_VAR(ack_level='ephemeral') ");
  EXPECT_EQ(one("ROLLBACK").kind, Kind::rollback);
  EXPECT_EQ(one("rollback work").kind, Kind::rollback);
}

TEST(SqlParser, ShowWarningsAndErrors) {
  auto s = one("SHOW WARNINGS");
  EXPECT_EQ(s.kind, Kind::show_warnings);
  EXPECT_FALSE(s.errors_only);
  EXPECT_FALSE(s.count_only);
  EXPECT_FALSE(s.select.limit);
  s = one("show errors limit 2");
  EXPECT_TRUE(s.errors_only);
  EXPECT_EQ(to_string(*s.select.limit), "2");
  s = one("SHOW WARNINGS LIMIT 1, 3");
  EXPECT_EQ(to_string(*s.select.offset), "1");
  EXPECT_EQ(to_string(*s.select.limit), "3");
  s = one("SHOW COUNT(*) WARNINGS");
  EXPECT_TRUE(s.count_only);
  EXPECT_FALSE(s.errors_only);
  EXPECT_TRUE(one("SHOW COUNT(*) ERRORS").errors_only);
  EXPECT_EQ(error_at("SHOW"), 4u);
  EXPECT_EQ(error_at("SHOW NOPE"), 5u);
  EXPECT_EQ(error_at("SHOW COUNT(1) WARNINGS"), 11u);
}

TEST(SqlParser, SeveralStatements) {
  const std::string text = "SELECT 1; SELECT 2 ;\nSET @a=1;";
  const auto statements = parse(text);
  ASSERT_EQ(statements.size(), 3u);
  EXPECT_EQ(text.substr(statements[0].offset, statements[0].length), "SELECT 1");
  EXPECT_EQ(text.substr(statements[1].offset, statements[1].length), "SELECT 2");
  EXPECT_EQ(text.substr(statements[2].offset, statements[2].length), "SET @a=1");
  EXPECT_EQ(statements[2].kind, Kind::set);
}

TEST(SqlParser, NothingToRunIsAnEmptyQuery) {
  EXPECT_THROW(parse(""), EmptyQuery);
  EXPECT_THROW(parse("  /* nothing */ -- here\n"), EmptyQuery);
  EXPECT_THROW(parse(";"), SyntaxError);
  EXPECT_THROW(parse("SELECT 1;;"), SyntaxError);
}

TEST(SqlParser, SyntaxErrorsPointWhereUnderstandingStopped) {
  EXPECT_EQ(error_at("SELEC 1"), 0u);
  EXPECT_EQ(error_at("SELECT"), 6u);
  EXPECT_EQ(error_at("SELECT 1 +"), 10u);
  EXPECT_EQ(error_at("SELECT select"), 7u);
  EXPECT_EQ(error_at("SELECT 1 FROM"), 13u);
  EXPECT_EQ(error_at("SELECT (1"), 9u);
  EXPECT_EQ(error_at("SELECT 1 2"), 9u);
  EXPECT_EQ(error_at("SELECT a FROM t WHERE"), 21u);
  EXPECT_EQ(error_at("SELECT 1 AS from"), 12u);
  EXPECT_EQ(error_at("SET"), 3u);
  EXPECT_EQ(error_at("SET a"), 5u);
  EXPECT_EQ(error_at("USE"), 3u);
  EXPECT_EQ(error_at("USE select"), 4u);
  EXPECT_EQ(error_at("COMMIT 1"), 7u);
  EXPECT_EQ(error_at("SELECT CASE WHEN 1 END"), 19u);
  EXPECT_EQ(error_at("SELECT a IN ()"), 13u);
  EXPECT_EQ(error_at("SELECT COUNT(DISTINCT *)"), 22u);
}

TEST(SqlParser, TheErrorQuotesTheRestOfTheText) {
  try {
    parse("SELECT 1 FROM WHERE x");
    FAIL();
  } catch (const SyntaxError& e) {
    EXPECT_STREQ(e.what(),
                 "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near "
                 "'WHERE x' at line 1");
  }
}

// The manual's CREATE DATABASE, DROP DATABASE and SHOW DATABASES.
TEST(SqlParser, DatabaseStatements) {
  auto s = one("CREATE DATABASE d");
  EXPECT_EQ(s.kind, Kind::create_database);
  EXPECT_EQ(s.database, "d");
  EXPECT_FALSE(s.if_exists);
  EXPECT_TRUE(s.charset.empty());
  s = one("CREATE SCHEMA IF NOT EXISTS `a b` DEFAULT CHARACTER SET = utf8mb4 DEFAULT COLLATE = utf8mb4_bin");
  EXPECT_EQ(s.kind, Kind::create_database);
  EXPECT_EQ(s.database, "a b");
  EXPECT_TRUE(s.if_exists);
  EXPECT_EQ(s.charset, "utf8mb4");
  EXPECT_EQ(s.collation, "utf8mb4_bin");
  s = one("CREATE DATABASE d CHARSET latin1 COLLATE 'x'");
  EXPECT_EQ(s.charset, "latin1");
  EXPECT_EQ(s.collation, "x");
  s = one("DROP DATABASE IF EXISTS d");
  EXPECT_EQ(s.kind, Kind::drop_database);
  EXPECT_TRUE(s.if_exists);
  s = one("DROP SCHEMA d");
  EXPECT_EQ(s.kind, Kind::drop_database);
  EXPECT_FALSE(s.if_exists);
  s = one("SHOW DATABASES");
  EXPECT_EQ(s.kind, Kind::show_databases);
  EXPECT_FALSE(s.like);
  s = one("SHOW SCHEMAS LIKE 'kv%'");
  EXPECT_EQ(s.kind, Kind::show_databases);
  EXPECT_EQ(s.like.value_or(""), "kv%");
  EXPECT_EQ(error_at("CREATE DATABASE"), 15u);
  EXPECT_EQ(error_at("DROP DATABASE IF d"), 17u);
  EXPECT_EQ(error_at("SHOW DATABASES LIKE 1"), 20u);
  EXPECT_EQ(error_at("CREATE DATABASE d NOPE"), 18u);
}

// The manual's CREATE TABLE ("CREATE TABLE Statement", "Data Type Syntax"):
// column definitions, PRIMARY KEY, UNIQUE and INDEX/KEY, table options.
TEST(SqlParser, CreateTable) {
  auto s = one("CREATE TABLE t (a INT)");
  EXPECT_EQ(s.kind, Kind::create_table);
  EXPECT_EQ(s.create_table.name, (std::vector<std::string>{"t"}));
  EXPECT_FALSE(s.create_table.if_not_exists);
  ASSERT_EQ(s.create_table.columns.size(), 1u);
  EXPECT_EQ(s.create_table.columns[0].name, "a");
  EXPECT_EQ(s.create_table.columns[0].type.name, "INT");
  EXPECT_FALSE(s.create_table.columns[0].nullable);
  EXPECT_TRUE(s.unsupported.empty());

  s =
      one("CREATE TABLE IF NOT EXISTS d.`t t` (a INTEGER(5) UNSIGNED ZEROFILL NOT NULL DEFAULT -3 AUTO_INCREMENT UNIQUE KEY COMMENT 'c', "
          "b DECIMAL(10,2), c NUMERIC, d DEC(4), e FIXED(3,1), f REAL, g DOUBLE PRECISION, h FLOAT(7,3), i BOOLEAN, j BOOL NULL)");
  const auto& t = s.create_table;
  EXPECT_EQ(t.name, (std::vector<std::string>{"d", "t t"}));
  EXPECT_TRUE(t.if_not_exists);
  ASSERT_EQ(t.columns.size(), 10u);
  const auto& a = t.columns[0];
  EXPECT_EQ(a.type.name, "INT");
  EXPECT_EQ(a.type.length, 5u);
  EXPECT_TRUE(a.type.is_unsigned);
  EXPECT_TRUE(a.type.zerofill);
  EXPECT_EQ(a.nullable, false);
  ASSERT_TRUE(a.default_value);
  EXPECT_EQ(to_string(*a.default_value), "(- 3)");
  EXPECT_TRUE(a.auto_increment);
  EXPECT_TRUE(a.unique);
  EXPECT_FALSE(a.primary);
  EXPECT_EQ(a.comment, "c");
  EXPECT_EQ(t.columns[1].type.name, "DECIMAL");
  EXPECT_EQ(t.columns[1].type.length, 10u);
  EXPECT_EQ(t.columns[1].type.scale, 2u);
  EXPECT_EQ(t.columns[2].type.name, "DECIMAL");
  EXPECT_FALSE(t.columns[2].type.length);
  EXPECT_EQ(t.columns[3].type.name, "DECIMAL");
  EXPECT_EQ(t.columns[4].type.name, "DECIMAL");
  EXPECT_EQ(t.columns[5].type.name, "DOUBLE");
  EXPECT_EQ(t.columns[6].type.name, "DOUBLE");
  EXPECT_EQ(t.columns[7].type.name, "FLOAT");
  EXPECT_EQ(t.columns[7].type.scale, 3u);
  EXPECT_EQ(t.columns[8].type.name, "BOOL");
  EXPECT_EQ(t.columns[9].nullable, true);

  s =
      one("CREATE TABLE t (a CHAR, b CHARACTER(3) CHARACTER SET utf8mb3 COLLATE utf8mb3_bin, c VARCHAR(5) CHARSET binary, d NATIONAL CHAR(2), "
          "e NCHAR VARCHAR(4), f NVARCHAR(3), g CHARACTER VARYING(2), h CHAR(3) BINARY, i VARBINARY(8), j BINARY(2), k TEXT(100), l TINYBLOB, "
          "m LONGTEXT COLLATE utf8mb4_bin, n SERIAL, o INT SERIAL DEFAULT VALUE, p INT KEY, q INT PRIMARY KEY, r INT DEFAULT NULL, s INT NOT NULL NULL)");
  const auto& u = s.create_table;
  ASSERT_EQ(u.columns.size(), 19u);
  EXPECT_EQ(u.columns[0].type.name, "CHAR");
  EXPECT_FALSE(u.columns[0].type.length);
  EXPECT_EQ(u.columns[1].type.name, "CHAR");
  EXPECT_EQ(u.columns[1].type.charset, "utf8mb3");
  EXPECT_EQ(u.columns[1].type.collation, "utf8mb3_bin");
  EXPECT_EQ(u.columns[2].type.charset, "binary");
  EXPECT_EQ(u.columns[3].type.name, "CHAR");
  EXPECT_EQ(u.columns[3].type.charset, "utf8mb3");
  EXPECT_EQ(u.columns[4].type.name, "VARCHAR");
  EXPECT_EQ(u.columns[4].type.charset, "utf8mb3");
  EXPECT_EQ(u.columns[5].type.name, "VARCHAR");
  EXPECT_EQ(u.columns[5].type.charset, "utf8mb3");
  EXPECT_EQ(u.columns[6].type.name, "VARCHAR");
  EXPECT_TRUE(u.columns[7].type.binary_attribute);
  EXPECT_EQ(u.columns[8].type.name, "VARBINARY");
  EXPECT_EQ(u.columns[9].type.name, "BINARY");
  EXPECT_EQ(u.columns[10].type.name, "TEXT");
  EXPECT_EQ(u.columns[10].type.length, 100u);
  EXPECT_EQ(u.columns[11].type.name, "TINYBLOB");
  EXPECT_EQ(u.columns[12].type.collation, "utf8mb4_bin");
  // SERIAL is BIGINT UNSIGNED NOT NULL AUTO_INCREMENT UNIQUE; SERIAL DEFAULT
  // VALUE is NOT NULL AUTO_INCREMENT UNIQUE.
  EXPECT_EQ(u.columns[13].type.name, "BIGINT");
  EXPECT_TRUE(u.columns[13].type.is_unsigned);
  EXPECT_EQ(u.columns[13].nullable, false);
  EXPECT_TRUE(u.columns[13].auto_increment);
  EXPECT_TRUE(u.columns[13].unique);
  EXPECT_EQ(u.columns[14].type.name, "INT");
  EXPECT_EQ(u.columns[14].nullable, false);
  EXPECT_TRUE(u.columns[14].auto_increment);
  EXPECT_TRUE(u.columns[14].unique);
  EXPECT_TRUE(u.columns[15].primary);
  EXPECT_TRUE(u.columns[16].primary);
  ASSERT_TRUE(u.columns[17].default_value);
  EXPECT_EQ(u.columns[17].default_value->kind, ExprKind::null);
  EXPECT_EQ(u.columns[18].nullable, true);

  s =
      one("CREATE TABLE t (a INT, b INT, PRIMARY KEY (a, b DESC), CONSTRAINT uq UNIQUE KEY (b), UNIQUE INDEX u2 USING BTREE (a(3)) COMMENT 'x', "
          "KEY k (a), INDEX (b ASC), CONSTRAINT pk2 PRIMARY KEY (b)) ENGINE = InnoDB, DEFAULT CHARSET=utf8mb4 COLLATE utf8mb4_bin AUTO_INCREMENT 5 "
          "COMMENT = 'tc'");
  const auto& k = s.create_table;
  ASSERT_EQ(k.keys.size(), 6u);
  EXPECT_EQ(k.keys[0].kind, KeySpec::Kind::primary);
  ASSERT_EQ(k.keys[0].parts.size(), 2u);
  EXPECT_EQ(k.keys[0].parts[1].column, "b");
  EXPECT_TRUE(k.keys[0].parts[1].descending);
  EXPECT_EQ(k.keys[1].kind, KeySpec::Kind::unique);
  EXPECT_EQ(k.keys[1].name, "uq");
  EXPECT_EQ(k.keys[2].name, "u2");
  EXPECT_EQ(k.keys[2].parts[0].prefix, 3u);
  EXPECT_EQ(k.keys[2].comment, "x");
  EXPECT_EQ(k.keys[3].kind, KeySpec::Kind::index);
  EXPECT_EQ(k.keys[3].name, "k");
  EXPECT_TRUE(k.keys[4].name.empty());
  EXPECT_EQ(k.keys[5].kind, KeySpec::Kind::primary);
  EXPECT_EQ(k.engine, "InnoDB");
  EXPECT_EQ(k.charset, "utf8mb4");
  EXPECT_EQ(k.collation, "utf8mb4_bin");
  EXPECT_EQ(k.auto_increment, 5u);
  EXPECT_EQ(k.comment, "tc");

  // A default that is not a literal is MySQL's to refuse by column type: a
  // niladic function is read.
  s = one("CREATE TABLE t (a INT DEFAULT CURRENT_TIMESTAMP, b INT DEFAULT TRUE, c CHAR(2) DEFAULT X'6162', d INT DEFAULT +4)");
  EXPECT_EQ(s.create_table.columns[0].default_value->kind, ExprKind::call);
  EXPECT_EQ(s.create_table.columns[1].default_value->kind, ExprKind::boolean);
  EXPECT_EQ(s.create_table.columns[2].default_value->kind, ExprKind::hex);
  EXPECT_EQ(to_string(*s.create_table.columns[3].default_value), "4");

  // Valid MySQL Kvasir does not yet carry out: marked, the rest unread.
  EXPECT_EQ(one("CREATE TABLE t2 LIKE t").unsupported, "CREATE TABLE ... LIKE");
  EXPECT_EQ(one("CREATE TABLE t2 AS SELECT 1").unsupported, "CREATE TABLE ... SELECT");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT) SELECT 1").unsupported, "CREATE TABLE ... SELECT");
  EXPECT_EQ(one("CREATE TEMPORARY TABLE t2 (a INT)").unsupported, "CREATE TEMPORARY TABLE");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT, CHECK (a > 0))").unsupported, "CHECK");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT CHECK (a > 0))").unsupported, "CHECK");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT, CONSTRAINT c FOREIGN KEY (a) REFERENCES u (a))").unsupported, "FOREIGN KEY");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT REFERENCES u (a))").unsupported, "REFERENCES");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT DEFAULT (1 + 1))").unsupported, "DEFAULT (expression)");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT AS (1) STORED)").unsupported, "generated columns");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT GENERATED ALWAYS AS (1))").unsupported, "generated columns");
  EXPECT_EQ(one("CREATE TABLE t2 (a DATE)").unsupported, "the DATE type");
  EXPECT_EQ(one("CREATE TABLE t2 (a ENUM('x'))").unsupported, "the ENUM type");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT, FULLTEXT (a))").unsupported, "FULLTEXT");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT, KEY ((a + 1)))").unsupported, "functional key parts");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT) ROW_FORMAT=DYNAMIC").unsupported, "table option ROW_FORMAT");
  EXPECT_EQ(one("CREATE TABLE t2 (a INT) PARTITION BY HASH (a)").unsupported, "PARTITION BY");

  EXPECT_EQ(error_at("CREATE TABLE t ()"), 16u);
  EXPECT_EQ(error_at("CREATE TABLE t (a VARCHAR)"), 25u);
  EXPECT_EQ(error_at("CREATE TABLE t (zz BLAH)"), 19u);
  EXPECT_EQ(error_at("CREATE TABLE t (a INT DEFAULT 1 + 1)"), 32u);
  EXPECT_EQ(error_at("CREATE TABLE t (a INT"), 21u);
  EXPECT_EQ(error_at("CREATE TABLE t (a INT, PRIMARY KEY a)"), 36u);
}

// The manual's INSERT: VALUES (or VALUE) with rows, or SET; DEFAULT as a
// value.
TEST(SqlParser, Insert) {
  auto s = one("INSERT INTO t (a, d) VALUES (1, 10), (DEFAULT, -2)");
  EXPECT_EQ(s.kind, Kind::insert);
  EXPECT_EQ(s.insert.table, (std::vector<std::string>{"t"}));
  EXPECT_TRUE(s.insert.has_columns);
  EXPECT_EQ(s.insert.columns, (std::vector<std::string>{"a", "d"}));
  ASSERT_EQ(s.insert.rows.size(), 2u);
  EXPECT_EQ(to_string(*s.insert.rows[0][1]), "10");
  EXPECT_EQ(s.insert.rows[1][0]->kind, ExprKind::default_value);
  EXPECT_EQ(to_string(*s.insert.rows[1][1]), "(- 2)");
  s = one("INSERT d.t VALUE (1)");
  EXPECT_EQ(s.insert.table, (std::vector<std::string>{"d", "t"}));
  EXPECT_FALSE(s.insert.has_columns);
  s = one("INSERT INTO t () VALUES (), ()");
  EXPECT_TRUE(s.insert.has_columns);
  EXPECT_TRUE(s.insert.columns.empty());
  ASSERT_EQ(s.insert.rows.size(), 2u);
  EXPECT_TRUE(s.insert.rows[0].empty());
  s = one("INSERT t SET a = 6, d = DEFAULT");
  EXPECT_EQ(s.insert.columns, (std::vector<std::string>{"a", "d"}));
  ASSERT_EQ(s.insert.rows.size(), 1u);
  EXPECT_EQ(s.insert.rows[0][1]->kind, ExprKind::default_value);
  s = one("INSERT LOW_PRIORITY INTO t VALUES (1)");
  EXPECT_TRUE(s.unsupported.empty());
  EXPECT_EQ(one("INSERT IGNORE INTO t VALUES (1)").unsupported, "INSERT IGNORE");
  EXPECT_EQ(one("INSERT INTO t VALUES (1) ON DUPLICATE KEY UPDATE a = 2").unsupported, "ON DUPLICATE KEY UPDATE");
  EXPECT_EQ(one("INSERT INTO t VALUES (1) AS n").unsupported, "INSERT ... AS");
  EXPECT_EQ(one("INSERT INTO t SELECT 1").unsupported, "INSERT ... SELECT");
  EXPECT_EQ(one("INSERT INTO t (a) SELECT 1").unsupported, "INSERT ... SELECT");
  EXPECT_EQ(one("INSERT INTO t TABLE u").unsupported, "INSERT ... TABLE");
  EXPECT_EQ(one("INSERT DELAYED INTO t VALUES (1)").unsupported, "INSERT DELAYED");
  EXPECT_EQ(one("REPLACE INTO t VALUES (1)").unsupported, "REPLACE");
  EXPECT_EQ(error_at("INSERT INTO t"), 13u);
  EXPECT_EQ(error_at("INSERT INTO t VALUES"), 20u);
  EXPECT_EQ(error_at("INSERT INTO t VALUES (1"), 23u);
  EXPECT_EQ(error_at("INSERT INTO t (a VALUES (1)"), 17u);
}

// The manual's single-table UPDATE and DELETE.
TEST(SqlParser, UpdateAndDelete) {
  auto s = one("UPDATE t SET a = a + 10, b = DEFAULT, x.c = 1 WHERE id <= 2 ORDER BY id DESC LIMIT 2");
  EXPECT_EQ(s.kind, Kind::update);
  EXPECT_EQ(s.change.table.name, (std::vector<std::string>{"t"}));
  ASSERT_EQ(s.change.set.size(), 3u);
  EXPECT_EQ(s.change.set[0].first, (std::vector<std::string>{"a"}));
  EXPECT_EQ(to_string(*s.change.set[0].second), "(+ a 10)");
  EXPECT_EQ(s.change.set[1].second->kind, ExprKind::default_value);
  EXPECT_EQ(s.change.set[2].first, (std::vector<std::string>{"x", "c"}));
  EXPECT_EQ(to_string(*s.change.where), "(<= id 2)");
  ASSERT_EQ(s.change.order.size(), 1u);
  EXPECT_TRUE(s.change.order[0].descending);
  EXPECT_EQ(to_string(*s.change.limit), "2");
  s = one("UPDATE LOW_PRIORITY d.t AS x SET x.a = 7");
  EXPECT_EQ(s.change.table.name, (std::vector<std::string>{"d", "t"}));
  EXPECT_EQ(s.change.table.alias, "x");
  EXPECT_FALSE(s.change.where);
  s = one("DELETE FROM t WHERE id = 2");
  EXPECT_EQ(s.kind, Kind::delete_);
  EXPECT_EQ(to_string(*s.change.where), "(= id 2)");
  s = one("DELETE LOW_PRIORITY QUICK FROM d.t ORDER BY id LIMIT 1");
  EXPECT_EQ(s.change.table.name, (std::vector<std::string>{"d", "t"}));
  EXPECT_EQ(s.change.order.size(), 1u);
  EXPECT_EQ(one("UPDATE IGNORE t SET a = 1").unsupported, "UPDATE IGNORE");
  EXPECT_EQ(one("UPDATE t, u SET t.a = 1").unsupported, "multiple-table UPDATE");
  EXPECT_EQ(one("UPDATE t JOIN u ON t.a = u.a SET t.a = 1").unsupported, "multiple-table UPDATE");
  EXPECT_EQ(one("DELETE IGNORE FROM t").unsupported, "DELETE IGNORE");
  EXPECT_EQ(one("DELETE t FROM t JOIN u").unsupported, "multiple-table DELETE");
  EXPECT_EQ(one("DELETE FROM t USING t JOIN u").unsupported, "multiple-table DELETE");
  EXPECT_EQ(one("DELETE FROM t, u USING t").unsupported, "multiple-table DELETE");
  EXPECT_EQ(error_at("UPDATE t"), 8u);
  EXPECT_EQ(error_at("UPDATE t SET"), 12u);
  EXPECT_EQ(error_at("UPDATE t SET a"), 14u);
  EXPECT_EQ(error_at("DELETE t"), 7u);
  EXPECT_EQ(error_at("DELETE FROM"), 11u);
}

// DROP TABLE and SHOW TABLES.
TEST(SqlParser, DropAndShowTables) {
  auto s = one("DROP TABLE t");
  EXPECT_EQ(s.kind, Kind::drop_table);
  EXPECT_EQ(s.tables, (std::vector<std::vector<std::string>>{{"t"}}));
  EXPECT_FALSE(s.if_exists);
  s = one("DROP TABLES IF EXISTS t, d.u RESTRICT");
  EXPECT_TRUE(s.if_exists);
  EXPECT_EQ(s.tables, (std::vector<std::vector<std::string>>{{"t"}, {"d", "u"}}));
  s = one("DROP TABLE t CASCADE");
  EXPECT_EQ(s.tables.size(), 1u);
  EXPECT_EQ(one("DROP TEMPORARY TABLE t").unsupported, "DROP TEMPORARY TABLE");
  s = one("SHOW TABLES");
  EXPECT_EQ(s.kind, Kind::show_tables);
  EXPECT_FALSE(s.full);
  EXPECT_TRUE(s.database.empty());
  EXPECT_FALSE(s.like);
  s = one("SHOW FULL TABLES FROM d LIKE 't%'");
  EXPECT_EQ(s.kind, Kind::show_tables);
  EXPECT_TRUE(s.full);
  EXPECT_EQ(s.database, "d");
  EXPECT_EQ(s.like.value_or(""), "t%");
  s = one("SHOW TABLES IN d");
  EXPECT_EQ(s.database, "d");
  EXPECT_EQ(one("SHOW TABLES WHERE 1").unsupported, "SHOW TABLES ... WHERE");
  EXPECT_EQ(error_at("DROP TABLE"), 10u);
  EXPECT_EQ(error_at("DROP TABLE t,"), 13u);
  EXPECT_EQ(error_at("SHOW FULL"), 9u);
}

}  // namespace
}  // namespace kvasir::sql
