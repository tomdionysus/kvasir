//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A session running the statements a client sends as it connects. Expected
// columns, values, refusals and status flags were observed on MySQL 8.4.13
// (Oracle's container on node-b, 2026-11-15/16) with a utf8mb4 client;
// the version strings are Kvasir's (the design, rulings 20 and 26).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kvasir/catalog/catalog.h"
#include "kvasir/exec/session.h"
#include "kvasir/protocol/types.h"

namespace kvasir::exec {
namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;

const VariableRegistry& registry() {
  static const VariableRegistry r("replicated", "8.4.13-kvasir", "Kvasir - The Distributed Relational Database (in memory, not durable)");
  return r;
}

// The node's catalog, which every session of a node shares.
catalog::Catalog& node_catalog() {
  static catalog::Catalog c;
  return c;
}

Session make(catalog::Catalog& c = node_catalog()) { return Session(registry(), c, SessionInfo{"root", "10.0.0.1", 7}); }

// Runs every statement in `sql`; the last one's outcome.
Outcome run(Session& s, const std::string& sql) {
  Outcome last;
  for (const auto& statement : sql::parse(sql, s.parser_options())) last = s.execute(statement);
  return last;
}

std::vector<std::string> row(const Outcome& o, size_t i = 0) {
  std::vector<std::string> out;
  for (const auto& v : o.result.value().rows.at(i)) out.push_back(v.value_or("NULL"));
  return out;
}

struct Refusal {
  uint16_t code = 0;
  std::string sqlstate;
  std::string message;
};

Refusal refusal(Session& s, const std::string& sql) {
  try {
    run(s, sql);
  } catch (const SqlError& e) {
    return {e.code(), e.sqlstate(), e.what()};
  }
  return {};
}

TEST(ExecSession, TheVersionCommentQuery) {
  auto s = make();
  const auto o = run(s, "select @@version_comment limit 1");
  ASSERT_TRUE(o.result);
  ASSERT_EQ(o.result->columns.size(), 1u);
  EXPECT_EQ(o.result->columns[0].name, "@@version_comment");
  EXPECT_EQ(o.result->columns[0].type.field_type, f::var_string);
  EXPECT_EQ(o.result->columns[0].type.collation, 255);
  EXPECT_EQ(o.result->columns[0].type.length, 87380u);
  ASSERT_EQ(o.result->rows.size(), 1u);
  EXPECT_EQ(row(o), (std::vector<std::string>{"Kvasir - The Distributed Relational Database (in memory, not durable)"}));
}

TEST(ExecSession, ConnectorQueries) {
  auto s = make();
  const auto o = run(s,
                     "SELECT @@session.auto_increment_increment AS auto_increment_increment, @@character_set_client AS character_set_client, "
                     "@@character_set_connection AS character_set_connection, @@character_set_results AS character_set_results, "
                     "@@collation_connection AS collation_connection, @@max_allowed_packet AS max_allowed_packet, @@sql_mode AS sql_mode, "
                     "@@system_time_zone AS system_time_zone, @@time_zone AS time_zone, @@transaction_isolation AS transaction_isolation, "
                     "@@wait_timeout AS wait_timeout");
  EXPECT_EQ(o.result->columns[0].name, "auto_increment_increment");
  EXPECT_EQ(row(o),
            (std::vector<std::string>{"1", "utf8mb4", "utf8mb4", "utf8mb4", "utf8mb4_0900_ai_ci", "67108864",
                                      "ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION",
                                      "UTC", "SYSTEM", "REPEATABLE-READ", "28800"}));
}

TEST(ExecSession, WhereAndLimitOnTheOneRow) {
  auto s = make();
  EXPECT_TRUE(run(s, "SELECT 1 LIMIT 0").result->rows.empty());
  EXPECT_TRUE(run(s, "SELECT 1, 2 LIMIT 1 OFFSET 1").result->rows.empty());
  EXPECT_TRUE(run(s, "SELECT 1, 2 LIMIT 1, 1").result->rows.empty());
  EXPECT_TRUE(run(s, "SELECT 1 WHERE 0").result->rows.empty());
  EXPECT_TRUE(run(s, "SELECT 1 WHERE NULL").result->rows.empty());
  EXPECT_EQ(run(s, "SELECT 1 FROM DUAL WHERE 1").result->rows.size(), 1u);
  EXPECT_EQ(run(s, "SELECT 1 LIMIT 5").result->rows.size(), 1u);
  EXPECT_EQ(run(s, "SELECT 1, 'a'").result->columns.size(), 2u);
}

TEST(ExecSession, SelectsThatNeedATableAreRefused) {
  auto s = make();
  auto r = refusal(s, "SELECT *");
  EXPECT_EQ(r.code, 1096);
  EXPECT_EQ(r.sqlstate, "HY000");
  EXPECT_EQ(r.message, "No tables used");
  r = refusal(s, "SELECT a FROM t");
  EXPECT_EQ(r.code, 1046);
  EXPECT_EQ(r.sqlstate, "3D000");
  EXPECT_EQ(r.message, "No database selected");
  r = refusal(s, "SELECT * FROM nope.t");
  EXPECT_EQ(r.code, 1049);
  EXPECT_EQ(r.message, "Unknown database 'nope'");
  r = refusal(s, "SELECT 1 ORDER BY nope");
  EXPECT_EQ(r.code, 1054);
  EXPECT_EQ(r.message, "Unknown column 'nope' in 'order clause'");
}

TEST(ExecSession, UseKnowsNoDatabaseYet) {
  auto s = make();
  const auto r = refusal(s, "USE nope");
  EXPECT_EQ(r.code, 1049);
  EXPECT_EQ(r.sqlstate, "42000");
  EXPECT_EQ(r.message, "Unknown database 'nope'");
  EXPECT_FALSE(s.database());
  const auto o = run(s, "SELECT DATABASE()");
  EXPECT_EQ(row(o), (std::vector<std::string>{"NULL"}));
}

TEST(ExecSession, UserVariablesKeepTheirTypes) {
  auto s = make();
  run(s, "SET @i = 1, @d = 1.5, @r = 1e0, @s = 'abc', @n = NULL, @b = 0x41, @u = 18446744073709551615");
  const auto o = run(s, "SELECT @i, @d, @r, @s, @n, @b, @u, @i + 1, @s + 0, @d + 1, @d * 2, @nope");
  const auto& cols = o.result->columns;
  EXPECT_EQ(row(o), (std::vector<std::string>{"1", "1.5", "1", "abc", "NULL", "A", "18446744073709551615", "2", "0", "2.500000000000000000000000000000",
                                              "3.000000000000000000000000000000", "NULL"}));
  EXPECT_EQ(cols[0].type.field_type, f::longlong);
  EXPECT_EQ(cols[0].type.length, 21u);
  EXPECT_EQ(cols[0].type.flags, c::binary | c::num);
  EXPECT_EQ(cols[1].type.field_type, f::newdecimal);
  EXPECT_EQ(cols[1].type.length, 67u);
  EXPECT_EQ(cols[1].type.decimals, 30);
  EXPECT_EQ(cols[2].type.field_type, f::double_);
  EXPECT_EQ(cols[2].type.length, 23u);
  EXPECT_EQ(cols[3].type.field_type, f::long_blob);
  EXPECT_EQ(cols[3].type.collation, 255);
  EXPECT_EQ(cols[3].type.length, 268435440u);
  EXPECT_EQ(cols[3].type.flags, 0);
  EXPECT_EQ(cols[4].type.field_type, f::medium_blob);
  EXPECT_EQ(cols[4].type.collation, 63);
  EXPECT_EQ(cols[4].type.length, 16777215u);
  EXPECT_EQ(cols[4].type.flags, c::binary);
  EXPECT_EQ(cols[5].type.field_type, f::medium_blob);
  EXPECT_EQ(cols[6].type.flags, c::unsigned_ | c::binary | c::num);
  EXPECT_EQ(cols[6].type.length, 21u);
  EXPECT_EQ(cols[7].type.length, 22u);
  EXPECT_EQ(cols[8].type.field_type, f::double_);
  EXPECT_EQ(cols[9].type.length, 68u);
  EXPECT_EQ(cols[10].type.length, 67u);
  EXPECT_EQ(cols[11].type.field_type, f::var_string);
  EXPECT_EQ(cols[11].type.collation, 63);
  EXPECT_EQ(cols[11].type.length, 65532u);
  EXPECT_EQ(cols[11].type.flags, c::binary);
}

TEST(ExecSession, SetEvaluatesEveryValueBeforeAssigningAny) {
  auto s = make();
  run(s, "SET @a = 1, @b = @a + 1");
  EXPECT_EQ(row(run(s, "SELECT @b")), (std::vector<std::string>{"NULL"}));
  run(s, "SET @b = @a + 1");
  EXPECT_EQ(row(run(s, "SELECT @b")), (std::vector<std::string>{"2"}));
}

TEST(ExecSession, UserVariableNamesIgnoreCase) {
  auto s = make();
  run(s, "SET @Mixed = 5");
  EXPECT_EQ(row(run(s, "SELECT @mixed, @MIXED")), (std::vector<std::string>{"5", "5"}));
}

TEST(ExecSession, SettingSystemVariables) {
  auto s = make();
  run(s, "SET autocommit = 0, sql_mode = 'ANSI_QUOTES', SESSION wait_timeout = 100");
  EXPECT_EQ(row(run(s, "SELECT @@autocommit, @@sql_mode, @@wait_timeout")), (std::vector<std::string>{"0", "ANSI_QUOTES", "100"}));
  run(s, "SET autocommit = ON");
  EXPECT_EQ(row(run(s, "SELECT @@autocommit")), (std::vector<std::string>{"1"}));
  // A bare word is the value's name.
  run(s, "SET autocommit = OFF");
  EXPECT_EQ(row(run(s, "SELECT @@autocommit")), (std::vector<std::string>{"0"}));
  run(s, "SET @@session.autocommit = DEFAULT");
  EXPECT_EQ(row(run(s, "SELECT @@autocommit")), (std::vector<std::string>{"1"}));
  EXPECT_EQ(refusal(s, "SET autocommit = 2").code, 1231);
  EXPECT_EQ(refusal(s, "SET nope = 1").code, 1193);
  EXPECT_EQ(refusal(s, "SELECT @@session.version_comment").code, 1238);
}

TEST(ExecSession, SqlModeChangesHowStatementsRead) {
  auto s = make();
  EXPECT_FALSE(s.parser_options().lexer.ansi_quotes);
  run(s, "SET sql_mode = 'ANSI'");
  EXPECT_TRUE(s.parser_options().lexer.ansi_quotes);
  EXPECT_TRUE(s.parser_options().pipes_as_concat);
  run(s, "SET sql_mode = 'NO_BACKSLASH_ESCAPES'");
  EXPECT_TRUE(s.parser_options().lexer.no_backslash_escapes);
  EXPECT_FALSE(s.parser_options().lexer.ansi_quotes);
}

TEST(ExecSession, SetNames) {
  auto s = make();
  run(s, "SET NAMES utf8mb4 COLLATE utf8mb4_bin");
  EXPECT_EQ(row(run(s, "SELECT @@character_set_client, @@character_set_results, @@collation_connection")),
            (std::vector<std::string>{"utf8mb4", "utf8mb4", "utf8mb4_bin"}));
  // String results are reported in the connection's collation.
  EXPECT_EQ(run(s, "SELECT 'a'").result->columns[0].type.collation, 46);
  EXPECT_EQ(run(s, "SELECT @@sql_mode").result->columns[0].type.collation, 46);
  run(s, "SET NAMES DEFAULT");
  EXPECT_EQ(row(run(s, "SELECT @@collation_connection")), (std::vector<std::string>{"utf8mb4_0900_ai_ci"}));
  run(s, "SET CHARACTER SET utf8mb4");
  EXPECT_EQ(row(run(s, "SELECT @@character_set_connection")), (std::vector<std::string>{"utf8mb4"}));
  auto r = refusal(s, "SET NAMES nope");
  EXPECT_EQ(r.code, 1115);
  EXPECT_EQ(r.message, "Unknown character set: 'nope'");
  EXPECT_EQ(refusal(s, "SET NAMES latin1").code, 1235);
  EXPECT_EQ(refusal(s, "SET NAMES utf8mb4 COLLATE utf8mb4_nope").code, 1273);
}

TEST(ExecSession, TransactionsShowInTheStatusFlags) {
  auto s = make();
  constexpr uint16_t in_trans = 1, autocommit = 2;
  EXPECT_EQ(s.status_flags(), autocommit);
  run(s, "BEGIN");
  EXPECT_EQ(s.status_flags(), in_trans | autocommit);
  run(s, "COMMIT");
  EXPECT_EQ(s.status_flags(), autocommit);
  run(s, "START TRANSACTION READ ONLY");
  EXPECT_TRUE(s.in_transaction());
  run(s, "ROLLBACK");
  EXPECT_FALSE(s.in_transaction());
  run(s, "SET autocommit = 0");
  EXPECT_EQ(s.status_flags(), 0);
  run(s, "BEGIN");
  // Turning autocommit on commits the transaction open.
  run(s, "SET autocommit = 1");
  EXPECT_FALSE(s.in_transaction());
  EXPECT_EQ(s.status_flags(), autocommit);
}

TEST(ExecSession, WarningsBelongToTheLastStatement) {
  auto s = make();
  run(s, "SELECT 1/0");
  ASSERT_EQ(s.warnings().size(), 1u);
  EXPECT_EQ(s.warnings()[0].code, 1365);
  run(s, "SELECT 1");
  EXPECT_TRUE(s.warnings().empty());
  run(s, "SET wait_timeout = 0");
  ASSERT_EQ(s.warnings().size(), 1u);
  EXPECT_EQ(s.warnings()[0].code, 1292);
}

TEST(ExecSession, ShowWarningsListsTheLastStatementsDiagnostics) {
  auto s = make();
  run(s, "SELECT 1/0, '1.5x'+0");
  auto o = run(s, "SHOW WARNINGS");
  const auto& cols = o.result->columns;
  ASSERT_EQ(cols.size(), 3u);
  EXPECT_EQ(cols[0].name, "Level");
  EXPECT_EQ(cols[0].type.field_type, f::var_string);
  EXPECT_EQ(cols[0].type.length, 28u);
  EXPECT_EQ(cols[0].type.flags, c::not_null);
  EXPECT_EQ(cols[1].name, "Code");
  EXPECT_EQ(cols[1].type.field_type, f::long_);
  EXPECT_EQ(cols[1].type.length, 5u);
  EXPECT_EQ(cols[1].type.flags, c::not_null | c::unsigned_ | c::binary | c::num);
  EXPECT_EQ(cols[2].name, "Message");
  EXPECT_EQ(cols[2].type.length, 2048u);
  ASSERT_EQ(o.result->rows.size(), 2u);
  EXPECT_EQ(row(o, 0), (std::vector<std::string>{"Warning", "1365", "Division by 0"}));
  EXPECT_EQ(row(o, 1), (std::vector<std::string>{"Warning", "1292", "Truncated incorrect DOUBLE value: '1.5x'"}));
  // SHOW WARNINGS leaves them where they are.
  EXPECT_EQ(run(s, "SHOW WARNINGS").result->rows.size(), 2u);
  EXPECT_EQ(run(s, "SHOW WARNINGS LIMIT 1").result->rows.size(), 1u);
  EXPECT_EQ(run(s, "SHOW WARNINGS LIMIT 1, 5").result->rows.size(), 1u);
  EXPECT_TRUE(run(s, "SHOW ERRORS").result->rows.empty());
  o = run(s, "SHOW COUNT(*) WARNINGS");
  EXPECT_EQ(o.result->columns[0].name, "@@session.warning_count");
  EXPECT_EQ(o.result->columns[0].type.length, 21u);
  EXPECT_EQ(row(o), (std::vector<std::string>{"2"}));
  EXPECT_EQ(row(run(s, "SELECT @@warning_count, @@error_count")), (std::vector<std::string>{"2", "0"}));
  run(s, "SELECT 1");
  EXPECT_TRUE(run(s, "SHOW WARNINGS").result->rows.empty());
}

TEST(ExecSession, AnErrorIsInTheDiagnostics) {
  auto s = make();
  EXPECT_EQ(refusal(s, "SELECT nope").code, 1054);
  auto o = run(s, "SHOW ERRORS");
  ASSERT_EQ(o.result->rows.size(), 1u);
  EXPECT_EQ(row(o), (std::vector<std::string>{"Error", "1054", "Unknown column 'nope' in 'field list'"}));
  EXPECT_EQ(run(s, "SHOW WARNINGS").result->rows.size(), 1u);
  EXPECT_EQ(row(run(s, "SHOW COUNT(*) ERRORS")), (std::vector<std::string>{"1"}));
  // A statement that never reached the session: the connection records it.
  s.record_error(1064, "You have an error");
  EXPECT_EQ(row(run(s, "SHOW ERRORS")), (std::vector<std::string>{"Error", "1064", "You have an error"}));
}

TEST(ExecSession, SessionFunctions) {
  auto s = make();
  run(s, "SET @x = 1");
  auto o =
      run(s, "SELECT CONNECTION_ID(), VERSION(), USER(), CURRENT_USER(), SESSION_USER(), SYSTEM_USER(), LAST_INSERT_ID(), ROW_COUNT(), SCHEMA(), CURRENT_USER");
  EXPECT_EQ(row(o), (std::vector<std::string>{"7", "8.4.13-kvasir", "root@10.0.0.1", "root@%", "root@10.0.0.1", "root@10.0.0.1", "0", "0", "NULL", "root@%"}));
  const auto& cols = o.result->columns;
  EXPECT_EQ(cols[0].type.field_type, f::longlong);
  EXPECT_EQ(cols[0].type.length, 21u);
  EXPECT_EQ(cols[0].type.flags, c::not_null | c::unsigned_ | c::binary | c::num);
  EXPECT_EQ(cols[1].type.field_type, f::var_string);
  EXPECT_EQ(cols[1].type.length, 52u);
  EXPECT_EQ(cols[1].type.flags, c::not_null);
  EXPECT_EQ(cols[2].type.length, 1152u);
  EXPECT_EQ(cols[2].type.flags, 0);
  EXPECT_EQ(cols[6].type.flags, c::not_null | c::unsigned_ | c::binary | c::num);
  EXPECT_EQ(cols[7].type.flags, c::not_null | c::binary | c::num);
  EXPECT_EQ(cols[7].type.length, 21u);
  EXPECT_EQ(cols[9].name, "CURRENT_USER");
  // After a SELECT, ROW_COUNT() is -1 and FOUND_ROWS() its rows.
  EXPECT_EQ(row(run(s, "SELECT ROW_COUNT(), FOUND_ROWS()")), (std::vector<std::string>{"-1", "1"}));
}

TEST(ExecSession, ColumnNames) {
  auto s = make();
  const auto o = run(s, "SELECT 1+1, 'abc', 1 AS one, @@version_comment, @a");
  std::vector<std::string> names;
  for (const auto& c : o.result->columns) names.push_back(c.name);
  EXPECT_EQ(names, (std::vector<std::string>{"1+1", "abc", "one", "@@version_comment", "@a"}));
}

// CREATE DATABASE, DROP DATABASE, SHOW DATABASES and USE, observed on MySQL
// 8.4.13, 2026-11-16 (lower_case_table_names = 0).
TEST(ExecSession, Databases) {
  catalog::Catalog node;
  auto s = make(node);
  auto other = make(node);
  const auto expect_refused = [&](const std::string& sql, uint16_t code, const std::string& state, const std::string& message) {
    SCOPED_TRACE(sql);
    const auto r = refusal(s, sql);
    EXPECT_EQ(r.code, code);
    EXPECT_EQ(r.sqlstate, state);
    EXPECT_EQ(r.message, message);
  };

  auto o = run(s, "CREATE DATABASE kv_probe");
  EXPECT_FALSE(o.result);
  EXPECT_EQ(o.affected_rows, 1u);
  expect_refused("CREATE DATABASE kv_probe", 1007, "HY000", "Can't create database 'kv_probe'; database exists");
  // IF NOT EXISTS: one row affected all the same, and a note.
  o = run(s, "CREATE DATABASE IF NOT EXISTS kv_probe");
  EXPECT_EQ(o.affected_rows, 1u);
  ASSERT_EQ(s.warnings().size(), 1u);
  EXPECT_EQ(s.warnings()[0].level, "Note");
  EXPECT_EQ(s.warnings()[0].code, 1007);
  run(s, "CREATE SCHEMA kv_Probe");
  run(s, "CREATE DATABASE kv_probe2 CHARACTER SET utf8mb4 COLLATE utf8mb4_bin");
  run(s, "CREATE DATABASE `kv.probe`");
  expect_refused("CREATE DATABASE kv_probe4 COLLATE nope", 1273, "HY000", "Unknown collation: 'nope'");
  expect_refused("CREATE DATABASE kv_probe5 CHARACTER SET utf8mb4 COLLATE latin1_swedish_ci", 1253, "42000",
                 "COLLATION 'latin1_swedish_ci' is not valid for CHARACTER SET 'utf8mb4'");
  expect_refused("CREATE DATABASE `kv probe `", 1102, "42000", "Incorrect database name 'kv probe '");
  const std::string long_name(65, 'a');
  expect_refused("CREATE DATABASE " + long_name, 1059, "42000", "Identifier name '" + long_name + "' is too long");
  expect_refused("CREATE DATABASE information_schema", 1044, "42000", "Access denied for user 'root'@'10.0.0.1' to database 'information_schema'");
  // Kvasir's own refusal: a character set it does not yet convert.
  expect_refused("CREATE DATABASE kv_probe3 DEFAULT CHARSET = latin1", 1235, "42000", "This version of Kvasir doesn't yet support 'character set latin1'");

  // Another session of the node sees them.
  run(other, "USE kv_probe");
  EXPECT_EQ(other.database(), "kv_probe");

  o = run(s, "SHOW DATABASES LIKE 'kv%'");
  ASSERT_TRUE(o.result);
  ASSERT_EQ(o.result->columns.size(), 1u);
  const auto& column = o.result->columns[0];
  EXPECT_EQ(column.name, "Database (kv%)");
  EXPECT_EQ(column.type.field_type, f::var_string);
  EXPECT_EQ(column.type.collation, 255);
  EXPECT_EQ(column.type.length, 256u);
  EXPECT_EQ(column.type.decimals, 0);
  EXPECT_EQ(column.type.flags, c::not_null | c::binary | c::no_default_value);
  std::vector<std::string> names;
  for (size_t i = 0; i < o.result->rows.size(); ++i) names.push_back(row(o, i).at(0));
  EXPECT_EQ(names, (std::vector<std::string>{"kv.probe", "kv_Probe", "kv_probe", "kv_probe2"}));
  o = run(s, "SHOW SCHEMAS LIKE 'kv_probe'");
  names.clear();
  for (size_t i = 0; i < o.result->rows.size(); ++i) names.push_back(row(o, i).at(0));
  EXPECT_EQ(names, (std::vector<std::string>{"kv.probe", "kv_probe"}));
  EXPECT_EQ(run(s, "SHOW DATABASES").result->columns[0].name, "Database");

  run(s, "USE kv_probe");
  expect_refused("SELECT * FROM kv_probe.t", 1146, "42S02", "Table 'kv_probe.t' doesn't exist");
  expect_refused("SELECT * FROM t", 1146, "42S02", "Table 'kv_probe.t' doesn't exist");
  o = run(s, "DROP DATABASE kv_probe");
  EXPECT_EQ(o.affected_rows, 0u);
  // Dropping the current database leaves none current.
  EXPECT_FALSE(s.database());
  expect_refused("DROP DATABASE kv_probe", 1008, "HY000", "Can't drop database 'kv_probe'; database doesn't exist");
  // Known difference: MySQL's OK packet counts one warning here but its
  // diagnostics area is left empty (SHOW WARNINGS lists nothing,
  // @@warning_count is 0); Kvasir keeps the note its OK packet counts.
  o = run(s, "DROP DATABASE IF EXISTS kv_probe");
  EXPECT_EQ(o.affected_rows, 0u);
  ASSERT_EQ(s.warnings().size(), 1u);
  EXPECT_EQ(s.warnings()[0].code, 1008);
  expect_refused("USE kv_probe", 1049, "42000", "Unknown database 'kv_probe'");
  run(s, "DROP SCHEMA kv_Probe");
  EXPECT_EQ(node.database_names(), (std::vector<std::string>{"kv.probe", "kv_probe2"}));
  EXPECT_EQ(node.database("kv_probe2")->collation, "utf8mb4_bin");
}

}  // namespace
}  // namespace kvasir::exec
