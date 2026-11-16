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

}  // namespace
}  // namespace kvasir::sql
