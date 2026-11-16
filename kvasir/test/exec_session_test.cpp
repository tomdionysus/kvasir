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

#include <ostream>
#include <string>
#include <vector>

#include "kvasir/catalog/catalog.h"
#include "kvasir/exec/session.h"
#include "kvasir/protocol/types.h"
#include "kvasir/txn/store.h"

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

// And its rows.
txn::Store& node_store() {
  static txn::Store s;
  return s;
}

Session make(catalog::Catalog& c = node_catalog(), txn::Store& rows = node_store()) { return Session(registry(), c, rows, SessionInfo{"root", "10.0.0.1", 7}); }

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

// A result column's definition, as the probe printed it: type, collation,
// length, decimals and flags.
struct Shape {
  uint8_t field_type;
  uint16_t collation;
  uint32_t length;
  uint8_t decimals;
  uint16_t flags;
  friend bool operator==(const Shape&, const Shape&) = default;
};

std::ostream& operator<<(std::ostream& out, const Shape& s) {
  return out << "{" << int(s.field_type) << ", " << s.collation << ", " << s.length << ", " << int(s.decimals) << ", " << s.flags << "}";
}

std::vector<Shape> shapes(const Outcome& o) {
  std::vector<Shape> out;
  for (const auto& col : o.result.value().columns) out.push_back({col.type.field_type, col.type.collation, col.type.length, col.type.decimals, col.type.flags});
  return out;
}

// A session on a node of its own, with database pt current.
struct TableBed {
  catalog::Catalog node;
  txn::Store rows;
  Session s = make(node, rows);
  TableBed() { run(s, "CREATE DATABASE pt; USE pt"); }
  // Another client of the same node.
  Session other() {
    auto o = make(node, rows);
    run(o, "USE pt");
    return o;
  }

  void expect_refused(const std::string& sql, uint16_t code, const std::string& state, const std::string& message) {
    SCOPED_TRACE(sql);
    const auto r = refusal(s, sql);
    EXPECT_EQ(r.code, code);
    EXPECT_EQ(r.sqlstate, state);
    EXPECT_EQ(r.message, message);
  }
  void expect_warnings(const std::vector<Warning>& expected) {
    ASSERT_EQ(s.warnings().size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(s.warnings()[i].level, expected[i].level) << i;
      EXPECT_EQ(s.warnings()[i].code, expected[i].code) << i;
      EXPECT_EQ(s.warnings()[i].message, expected[i].message) << i;
    }
  }
};

constexpr uint16_t bin = 63, ai = 255;
constexpr uint16_t num = c::num, nn = c::not_null, pri = c::primary_key, uni = c::unique_key, mul = c::multiple_key, part = c::part_key,
                   nodef = c::no_default_value, autoinc = c::auto_increment, uns = c::unsigned_, zf = c::zerofill, blob = c::blob, binflag = c::binary;

// The numeric types' columns: MySQL 8.4.13, 2026-11-16.
TEST(ExecSessionTables, NumericColumns) {
  TableBed b;
  auto o = run(b.s,
               "CREATE TABLE t2 (a TINYINT, b SMALLINT, c MEDIUMINT, d INT, e BIGINT, f TINYINT UNSIGNED, g INT UNSIGNED ZEROFILL, "
               "h BIGINT UNSIGNED, i INT(5), j BOOL, k BOOLEAN, l INTEGER)");
  EXPECT_FALSE(o.result);
  EXPECT_EQ(o.affected_rows, 0u);
  o = run(b.s, "SELECT * FROM t2");
  EXPECT_TRUE(o.result->rows.empty());
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::tiny, bin, 4, 0, num},
                                           {f::short_, bin, 6, 0, num},
                                           {f::int24, bin, 9, 0, num},
                                           {f::long_, bin, 11, 0, num},
                                           {f::longlong, bin, 20, 0, num},
                                           {f::tiny, bin, 3, 0, uns | num},
                                           {f::long_, bin, 10, 0, uns | zf | num},
                                           {f::longlong, bin, 20, 0, uns | num},
                                           {f::long_, bin, 5, 0, num},
                                           {f::tiny, bin, 1, 0, num},
                                           {f::tiny, bin, 1, 0, num},
                                           {f::long_, bin, 11, 0, num}}));
  const auto& first = o.result->columns[0];
  EXPECT_EQ(first.name, "a");
  EXPECT_EQ(first.schema, "pt");
  EXPECT_EQ(first.table, "t2");
  EXPECT_EQ(first.original_table, "t2");
  EXPECT_EQ(first.original_name, "a");

  run(b.s,
      "CREATE TABLE t3 (a DECIMAL, b DECIMAL(10,2), c DECIMAL(65,30), d NUMERIC(5), e FLOAT, f DOUBLE, g FLOAT(7,3), h DOUBLE(10,4), i REAL, "
      "j DECIMAL(5,2) UNSIGNED, k FLOAT(30), l DOUBLE PRECISION, m DEC(4,1), n FIXED(3), o DECIMAL(0), p FLOAT(255,30), q FLOAT(24), r FLOAT(0))");
  o = run(b.s, "SELECT * FROM t3");
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::newdecimal, bin, 11, 0, num},
                                           {f::newdecimal, bin, 12, 2, num},
                                           {f::newdecimal, bin, 67, 30, num},
                                           {f::newdecimal, bin, 6, 0, num},
                                           {f::float_, bin, 12, 31, num},
                                           {f::double_, bin, 22, 31, num},
                                           {f::float_, bin, 7, 3, num},
                                           {f::double_, bin, 10, 4, num},
                                           {f::double_, bin, 22, 31, num},
                                           {f::newdecimal, bin, 6, 2, uns | num},
                                           {f::double_, bin, 22, 31, num},
                                           {f::double_, bin, 22, 31, num},
                                           {f::newdecimal, bin, 6, 1, num},
                                           {f::newdecimal, bin, 4, 0, num},
                                           {f::newdecimal, bin, 11, 0, num},
                                           {f::float_, bin, 255, 30, num},
                                           {f::float_, bin, 12, 31, num},
                                           {f::float_, bin, 12, 31, num}}));
}

TEST(ExecSessionTables, StringColumns) {
  TableBed b;
  run(b.s,
      "CREATE TABLE t4 (a CHAR, b CHAR(10), c VARCHAR(20), d BINARY(4), e VARBINARY(8), f TEXT, g TINYTEXT, h MEDIUMTEXT, i LONGTEXT, j BLOB, "
      "k TINYBLOB, l MEDIUMBLOB, m LONGBLOB, n VARCHAR(5) CHARACTER SET utf8mb3, o VARCHAR(5) COLLATE utf8mb4_bin, p CHAR(3) BINARY, q TEXT(100), "
      "r BLOB(300), s VARCHAR(10) CHARACTER SET binary, t NATIONAL CHAR(2), u NCHAR(2), v NVARCHAR(3), w CHAR(0))");
  const auto o = run(b.s, "SELECT * FROM t4");
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::string, ai, 4, 0, 0},
                                           {f::string, ai, 40, 0, 0},
                                           {f::var_string, ai, 80, 0, 0},
                                           {f::string, bin, 4, 0, binflag},
                                           {f::var_string, bin, 8, 0, binflag},
                                           {f::blob, ai, 262140, 0, blob},
                                           {f::blob, ai, 1020, 0, blob},
                                           {f::blob, ai, 67108860, 0, blob},
                                           {f::blob, ai, 4294967295u, 0, blob},
                                           {f::blob, bin, 65535, 0, blob | binflag},
                                           {f::blob, bin, 255, 0, blob | binflag},
                                           {f::blob, bin, 16777215, 0, blob | binflag},
                                           {f::blob, bin, 4294967295u, 0, blob | binflag},
                                           {f::var_string, ai, 20, 0, 0},
                                           {f::var_string, ai, 20, 0, binflag},
                                           {f::string, ai, 12, 0, binflag},
                                           {f::blob, ai, 262140, 0, blob},
                                           {f::blob, bin, 65535, 0, blob | binflag},
                                           {f::var_string, bin, 10, 0, binflag},
                                           {f::string, ai, 8, 0, 0},
                                           {f::string, ai, 8, 0, 0},
                                           {f::var_string, ai, 12, 0, 0},
                                           {f::string, ai, 0, 0, 0}}));
}

// NOT NULL, DEFAULT, AUTO_INCREMENT and keys, in the flags.
TEST(ExecSessionTables, KeysAndDefaultsInTheFlags) {
  TableBed b;
  run(b.s,
      "CREATE TABLE t5 (a INT NOT NULL, b INT NULL, c INT DEFAULT 5, d INT NOT NULL DEFAULT 7, e INT AUTO_INCREMENT PRIMARY KEY, "
      "f VARCHAR(3) DEFAULT 'x', g INT UNIQUE, h INT COMMENT 'hi', i INT DEFAULT NULL)");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM t5")), (std::vector<Shape>{{f::long_, bin, 11, 0, nn | nodef | num},
                                                                      {f::long_, bin, 11, 0, num},
                                                                      {f::long_, bin, 11, 0, num},
                                                                      {f::long_, bin, 11, 0, nn | num},
                                                                      {f::long_, bin, 11, 0, nn | pri | autoinc | num | part},
                                                                      {f::var_string, ai, 12, 0, 0},
                                                                      {f::long_, bin, 11, 0, uni | num | part},
                                                                      {f::long_, bin, 11, 0, num},
                                                                      {f::long_, bin, 11, 0, num}}));
  run(b.s, "CREATE TABLE t6 (a INT, b INT, PRIMARY KEY (a, b), UNIQUE KEY u (b), KEY k (a), INDEX (b))");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM t6")), (std::vector<Shape>{{f::long_, bin, 11, 0, nn | pri | mul | nodef | num | part},
                                                                      {f::long_, bin, 11, 0, nn | pri | uni | mul | nodef | num | part}}));
  // The first UNIQUE key over NOT NULL columns serves as the primary key.
  run(b.s, "CREATE TABLE u2 (a INT NOT NULL UNIQUE, b INT)");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM u2")), (std::vector<Shape>{{f::long_, bin, 11, 0, nn | pri | nodef | num | part}, {f::long_, bin, 11, 0, num}}));
  run(b.s, "CREATE TABLE u3 (a INT NOT NULL, b INT NOT NULL, UNIQUE (a, b))");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM u3")),
            (std::vector<Shape>{{f::long_, bin, 11, 0, nn | pri | nodef | num | part}, {f::long_, bin, 11, 0, nn | pri | nodef | num | part}}));
  run(b.s, "CREATE TABLE u4 (a INT, b INT, UNIQUE (a, b), KEY (b, a))");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM u4")), (std::vector<Shape>{{f::long_, bin, 11, 0, mul | num | part}, {f::long_, bin, 11, 0, mul | num | part}}));
  run(b.s, "CREATE TABLE u5 (a INT UNIQUE KEY, b INT NOT NULL UNIQUE, c INT NOT NULL PRIMARY KEY)");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM u5")), (std::vector<Shape>{{f::long_, bin, 11, 0, uni | num | part},
                                                                      {f::long_, bin, 11, 0, nn | uni | nodef | num | part},
                                                                      {f::long_, bin, 11, 0, nn | pri | nodef | num | part}}));
  run(b.s, "CREATE TABLE u26 (a VARCHAR(10), KEY (a(3)))");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM u26")), (std::vector<Shape>{{f::var_string, ai, 40, 0, mul | part}}));
  run(b.s, "CREATE TABLE u31 (a SERIAL)");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM u31")), (std::vector<Shape>{{f::longlong, bin, 20, 0, nn | pri | uns | autoinc | num | part}}));
  run(b.s, "CREATE TABLE u32 (a INT SERIAL DEFAULT VALUE)");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM u32")), (std::vector<Shape>{{f::long_, bin, 11, 0, nn | pri | autoinc | num | part}}));
  // AUTO_INCREMENT makes a column NOT NULL; it need only begin a key.
  run(b.s, "CREATE TABLE w29 (a INT AUTO_INCREMENT, KEY (a))");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM w29")), (std::vector<Shape>{{f::long_, bin, 11, 0, nn | mul | autoinc | num | part}}));
  run(b.s, "CREATE TABLE w31 (a INT, b INT AUTO_INCREMENT, KEY (b, a))");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM w31")),
            (std::vector<Shape>{{f::long_, bin, 11, 0, num | part}, {f::long_, bin, 11, 0, nn | mul | autoinc | num | part}}));
  // NOT NULL then NULL: the last one written.
  run(b.s, "CREATE TABLE w26 (a INT NOT NULL NULL)");
  EXPECT_EQ(shapes(run(b.s, "SELECT * FROM w26")), (std::vector<Shape>{{f::long_, bin, 11, 0, num}}));
}

// Defaults that fit, and the notes some give.
TEST(ExecSessionTables, Defaults) {
  TableBed b;
  for (const char* sql :
       {"CREATE TABLE w14 (a VARCHAR(5) DEFAULT 'ab   ')", "CREATE TABLE w17 (a INT DEFAULT ' 12 ')",
        "CREATE TABLE w18 (a INT DEFAULT 2.5, b INT DEFAULT -2.5, c INT DEFAULT 2.5e0, d INT DEFAULT 3.5e0, e INT DEFAULT '2.5')",
        "CREATE TABLE w22 (a TINYINT UNSIGNED DEFAULT 255, b TINYINT DEFAULT -128, c BIGINT UNSIGNED DEFAULT 18446744073709551615, "
        "d BIGINT DEFAULT -9223372036854775808)",
        "CREATE TABLE w24 (a BINARY(3) DEFAULT 'ab')", "CREATE TABLE w27 (a CHAR(2) DEFAULT X'6162')", "CREATE TABLE w28 (a INT DEFAULT X'01')",
        "CREATE TABLE u8 (a CHAR(2) DEFAULT 'ab ')", "CREATE TABLE u10 (a INT DEFAULT 1.5)", "CREATE TABLE w35 (a VARCHAR(3) DEFAULT 'é')",
        "CREATE TABLE w36 (a INT UNSIGNED ZEROFILL DEFAULT 5)",
        "CREATE TABLE u6 (b INT DEFAULT -3, c DECIMAL(5,2) DEFAULT 1.23, d VARCHAR(5) DEFAULT 12, e DOUBLE DEFAULT '1e3', f INT DEFAULT TRUE)"}) {
    SCOPED_TRACE(sql);
    run(b.s, sql);
    if (std::string(sql).find("ZEROFILL") == std::string::npos) {
      EXPECT_TRUE(b.s.warnings().empty());
    }
  }
  run(b.s, "CREATE TABLE u7 (a DECIMAL(5,2) DEFAULT 1.234)");
  b.expect_warnings({{"Note", 1265, "Data truncated for column 'a' at row 1"}});
  run(b.s, "CREATE TABLE w20 (a DECIMAL(3,1) DEFAULT 99.94)");
  b.expect_warnings({{"Note", 1265, "Data truncated for column 'a' at row 1"}});
  run(b.s, "CREATE TABLE w15 (a VARCHAR(2) DEFAULT 'ab ')");
  b.expect_warnings({{"Note", 1265, "Data truncated for column 'a' at row 1"}});

  const std::string invalid = "Invalid default value for ";
  b.expect_refused("CREATE TABLE t20 (a INT DEFAULT 'x')", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE t21 (a INT NOT NULL DEFAULT NULL)", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE w25 (a INT DEFAULT NULL NOT NULL)", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE t24 (a TINYINT DEFAULT 300)", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE t25 (a VARCHAR(2) DEFAULT 'abc')", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE u9 (a INT UNSIGNED DEFAULT -1)", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE w16 (a INT DEFAULT '12abc')", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE w19 (a DECIMAL(3,1) DEFAULT 99.95)", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE w21 (a FLOAT DEFAULT 1e40)", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE w23 (a BINARY(3) DEFAULT 'ab', b VARBINARY(2) DEFAULT 'abc')", 1067, "42000", invalid + "'b'");
  b.expect_refused("CREATE TABLE u35 (a INT AUTO_INCREMENT DEFAULT 3 KEY)", 1067, "42000", invalid + "'a'");
  b.expect_refused("CREATE TABLE u6b (a INT DEFAULT 1, g INT DEFAULT CURRENT_TIMESTAMP)", 1067, "42000", invalid + "'g'");
  b.expect_refused("CREATE TABLE t22 (a TEXT DEFAULT 'x')", 1101, "42000", "BLOB, TEXT, GEOMETRY or JSON column 'a' can't have a default value");
}

// MySQL's deprecation warnings, one per column in the order written
// (TINYINT(1) warns too).
TEST(ExecSessionTables, DeprecationWarnings) {
  TableBed b;
  run(b.s,
      "CREATE TABLE w1 (a INT(5), b INT ZEROFILL, c FLOAT(7,3), d DOUBLE(10,2), e DECIMAL(5,2) UNSIGNED, f FLOAT UNSIGNED, g TINYINT(1), "
      "h BIGINT(20), i INT(11))");
  const Warning width{"Warning", 1681, "Integer display width is deprecated and will be removed in a future release."};
  const Warning zerofill{"Warning", 1681,
                         "The ZEROFILL attribute is deprecated and will be removed in a future release. Use the LPAD function to zero-pad numbers, or "
                         "store the formatted numbers in a CHAR column."};
  const Warning digits{"Warning", 1681, "Specifying number of digits for floating point data types is deprecated and will be removed in a future release."};
  const Warning unsigned_{"Warning", 1681,
                          "UNSIGNED for decimal and floating point data types is deprecated and support for it will be removed in a future release."};
  b.expect_warnings({width, zerofill, digits, digits, unsigned_, unsigned_, width, width, width});
}

TEST(ExecSessionTables, DefinitionsMySqlRefuses) {
  TableBed b;
  run(b.s, "CREATE TABLE t1 (a INT)");
  b.expect_refused("CREATE TABLE t1 (a INT)", 1050, "42S01", "Table 't1' already exists");
  // IF NOT EXISTS: a note, before the definition is looked at.
  run(b.s, "CREATE TABLE IF NOT EXISTS t1 (a INT, a INT)");
  b.expect_warnings({{"Note", 1050, "Table 't1' already exists"}});
  b.expect_refused("CREATE TABLE t11 (a INT, a INT)", 1060, "42S21", "Duplicate column name 'a'");
  b.expect_refused("CREATE TABLE t11 (a INT, A INT)", 1060, "42S21", "Duplicate column name 'A'");
  b.expect_refused("CREATE TABLE t13 (a INT PRIMARY KEY, b INT PRIMARY KEY)", 1068, "42000", "Multiple primary key defined");
  b.expect_refused("CREATE TABLE t29 (a INT, PRIMARY KEY (a), PRIMARY KEY (a))", 1068, "42000", "Multiple primary key defined");
  b.expect_refused("CREATE TABLE t5 (e INT PRIMARY KEY, i INT KEY)", 1068, "42000", "Multiple primary key defined");
  const std::string auto_column = "Incorrect table definition; there can be only one auto column and it must be defined as a key";
  b.expect_refused("CREATE TABLE t14 (a INT AUTO_INCREMENT)", 1075, "42000", auto_column);
  b.expect_refused("CREATE TABLE t15 (a INT AUTO_INCREMENT KEY, b INT AUTO_INCREMENT UNIQUE)", 1075, "42000", auto_column);
  b.expect_refused("CREATE TABLE w30 (a INT, b INT AUTO_INCREMENT, PRIMARY KEY (a, b))", 1075, "42000", auto_column);
  b.expect_refused("CREATE TABLE u33 (a FLOAT AUTO_INCREMENT KEY)", 1063, "42000", "Incorrect column specifier for column 'a'");
  b.expect_refused("CREATE TABLE u34 (a VARCHAR(5) AUTO_INCREMENT KEY)", 1063, "42000", "Incorrect column specifier for column 'a'");
  b.expect_refused("CREATE TABLE t30 (a FLOAT(54))", 1063, "42000", "Incorrect column specifier for column 'a'");
  b.expect_refused("CREATE TABLE t16 (a VARCHAR(70000))", 1074, "42000", "Column length too big for column 'a' (max = 16383); use BLOB or TEXT instead");
  b.expect_refused("CREATE TABLE t32 (a VARCHAR(16384))", 1074, "42000", "Column length too big for column 'a' (max = 16383); use BLOB or TEXT instead");
  b.expect_refused("CREATE TABLE t17 (a CHAR(256))", 1074, "42000", "Column length too big for column 'a' (max = 255); use BLOB or TEXT instead");
  b.expect_refused("CREATE TABLE w42 (a VARCHAR(70000) CHARACTER SET binary)", 1074, "42000",
                   "Column length too big for column 'a' (max = 65535); use BLOB or TEXT instead");
  b.expect_refused("CREATE TABLE t18 (a DECIMAL(66))", 1426, "42000", "Too-big precision 66 specified for 'a'. Maximum is 65.");
  b.expect_refused("CREATE TABLE t19 (a DECIMAL(5,6))", 1427, "42000", "For float(M,D), double(M,D) or decimal(M,D), M must be >= D (column 'a').");
  b.expect_refused("CREATE TABLE w3 (a DECIMAL(5,31))", 1425, "42000", "Too big scale 31 specified for column 'a'. Maximum is 30.");
  b.expect_refused("CREATE TABLE w6 (a FLOAT(255,31))", 1425, "42000", "Too big scale 31 specified for column 'a'. Maximum is 30.");
  b.expect_refused("CREATE TABLE w5 (a FLOAT(256,2))", 1439, "42000", "Display width out of range for column 'a' (max = 255)");
  b.expect_refused("CREATE TABLE t31 (a INT(300))", 1439, "42000", "Display width out of range for column 'a' (max = 255)");
  b.expect_refused("CREATE TABLE nodb.t (a INT)", 1049, "42000", "Unknown database 'nodb'");
  b.expect_refused("CREATE TABLE t26 (a INT PRIMARY KEY, KEY (zz))", 1072, "42000", "Key column 'zz' doesn't exist in table");
  b.expect_refused("CREATE TABLE `t27 ` (a INT)", 1103, "42000", "Incorrect table name 't27 '");
  b.expect_refused("CREATE TABLE `` (a INT)", 1103, "42000", "Incorrect table name ''");
  b.expect_refused("CREATE TABLE t28 (`a ` INT)", 1166, "42000", "Incorrect column name 'a '");
  b.expect_refused("CREATE TABLE w37 (`` INT)", 1166, "42000", "Incorrect column name ''");
  const std::string long_name(65, 'a');
  b.expect_refused("CREATE TABLE " + long_name + " (a INT)", 1059, "42000", "Identifier name '" + long_name + "' is too long");
  b.expect_refused("CREATE TABLE u23 (" + long_name + " INT)", 1059, "42000", "Identifier name '" + long_name + "' is too long");
  b.expect_refused("CREATE TABLE u27 (a TEXT, PRIMARY KEY (a))", 1170, "42000", "BLOB/TEXT column 'a' used in key specification without a key length");
  b.expect_refused("CREATE TABLE u29 (a INT NULL PRIMARY KEY)", 1171, "42000",
                   "All parts of a PRIMARY KEY must be NOT NULL; if you need NULL in a key, use UNIQUE instead");
  b.expect_refused("CREATE TABLE w9 (a INT, KEY k (a), KEY k (a))", 1061, "42000", "Duplicate key name 'k'");
  b.expect_refused("CREATE TABLE w34 (a INT UNIQUE, b INT, KEY a (b))", 1061, "42000", "Duplicate key name 'a'");
  b.expect_refused("CREATE TABLE x12 (a INT, KEY (a), b INT, KEY a (b))", 1061, "42000", "Duplicate key name 'a'");
  b.expect_refused("CREATE TABLE w32 (a INT, `PRIMARY` INT, KEY `PRIMARY` (a))", 1280, "42000", "Incorrect index name 'PRIMARY'");
  const std::string prefix =
      "Incorrect prefix key; the used key part isn't a string, the used length is longer than the key part, or the storage engine doesn't support unique "
      "prefix keys";
  b.expect_refused("CREATE TABLE w10 (a INT, KEY (a(2)))", 1089, "HY000", prefix);
  b.expect_refused("CREATE TABLE w11 (a VARCHAR(5), KEY (a(6)))", 1089, "HY000", prefix);
  b.expect_refused("CREATE TABLE w12 (a VARCHAR(1000), KEY (a))", 1071, "42000", "Specified key was too long; max key length is 3072 bytes");
  const std::string row_size =
      "Row size too large. The maximum row size for the used table type, not counting BLOBs, is 65535. This includes storage overhead, check the manual. "
      "You have to change some columns to TEXT or BLOBs";
  b.expect_refused("CREATE TABLE w44 (a VARBINARY(65533))", 1118, "42000", row_size);
  b.expect_refused("CREATE TABLE w45 (a VARCHAR(16383), b INT)", 1118, "42000", row_size);
  b.expect_refused("CREATE TABLE y2 (a VARBINARY(65471), b TEXT, c LONGBLOB, d DECIMAL(20,5), e CHAR(10) CHARACTER SET binary, f TINYBLOB, g MEDIUMTEXT)", 1118,
                   "42000", row_size);
  b.expect_refused("CREATE TABLE y4 (a VARBINARY(65532) NOT NULL, b TINYINT)", 1118, "42000", row_size);
  b.expect_refused("CREATE TABLE u11 (a VARCHAR(5) CHARACTER SET utf8mb3 COLLATE utf8mb4_bin)", 1253, "42000",
                   "COLLATION 'utf8mb4_bin' is not valid for CHARACTER SET 'utf8mb3'");
  b.expect_refused("CREATE TABLE w41 (a INT) CHARSET utf8mb4 COLLATE utf8mb3_bin", 1253, "42000",
                   "COLLATION 'utf8mb3_bin' is not valid for CHARACTER SET 'utf8mb4'");
  b.expect_refused("CREATE TABLE u12 (a VARCHAR(5) CHARACTER SET nope)", 1115, "42000", "Unknown character set: 'nope'");
  b.expect_refused("CREATE TABLE u13 (a VARCHAR(5) COLLATE nope)", 1273, "HY000", "Unknown collation: 'nope'");
  b.expect_refused("CREATE TABLE t9 (a INT) ENGINE=Nope", 1286, "42000", "Unknown storage engine 'Nope'");
  b.expect_refused("CREATE TABLE information_schema.x (a INT)", 1044, "42000", "Access denied for user 'root'@'10.0.0.1' to database 'information_schema'");
  // What fits, just.
  for (const char* sql :
       {"CREATE TABLE w7 (a FLOAT(255,30))", "CREATE TABLE w13 (a VARCHAR(768), KEY (a))", "CREATE TABLE w43 (a VARBINARY(65532))",
        "CREATE TABLE y5 (a VARBINARY(65533) NOT NULL)",
        "CREATE TABLE x4 (a VARBINARY(65525), b TINYINT, c TINYINT, d TINYINT, e TINYINT, "
        "f TINYINT, g TINYINT, h TINYINT)",
        "CREATE TABLE y1 (a VARBINARY(65470), b TEXT, c LONGBLOB, d DECIMAL(20,5), e CHAR(10) CHARACTER SET binary, f TINYBLOB, g MEDIUMTEXT)",
        "CREATE TABLE w46 (a VARCHAR(16000), b VARCHAR(10))", "CREATE TABLE t7 (a INT) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci",
        "CREATE TABLE u14 (a INT) ENGINE=innodb", "CREATE TABLE u17 (a INT) ENGINE = InnoDB, AUTO_INCREMENT = 5, COMMENT 'c'",
        "CREATE TABLE t33 (a VARCHAR(16383))", "CREATE TABLE T1 (a INT)"}) {
    SCOPED_TRACE(sql);
    EXPECT_EQ(refusal(b.s, sql).code, 0);
  }
  // Kvasir's own refusals: what it does not yet carry out.
  b.expect_refused("CREATE TABLE u18 LIKE t1", 1235, "42000", "This version of Kvasir doesn't yet support 'CREATE TABLE ... LIKE'");
  b.expect_refused("CREATE TABLE u24 (f DATE)", 1235, "42000", "This version of Kvasir doesn't yet support 'the DATE type'");
  b.expect_refused("CREATE TABLE u36 (a INT) CHARSET latin1", 1235, "42000", "This version of Kvasir doesn't yet support 'character set latin1'");
  b.expect_refused("CREATE TABLE u37 (a VARCHAR(3) CHARACTER SET latin1)", 1235, "42000", "This version of Kvasir doesn't yet support 'character set latin1'");
}

// An engine other than InnoDB: with NO_ENGINE_SUBSTITUTION (the default) it
// is refused as unknown; without, InnoDB is used with two warnings.
TEST(ExecSessionTables, EngineSubstitution) {
  TableBed b;
  run(b.s, "SET sql_mode = ''");
  run(b.s, "CREATE TABLE u16 (a INT) ENGINE=Nope");
  b.expect_warnings({{"Warning", 1286, "Unknown storage engine 'Nope'"}, {"Warning", 1266, "Using storage engine InnoDB for table 'u16'"}});
  EXPECT_EQ(b.node.table({"pt", "u16"})->engine, "InnoDB");
}

TEST(ExecSessionTables, DuplicateIndexesWarn) {
  TableBed b;
  run(b.s, "CREATE TABLE u28 (a INT, KEY (a), KEY (a))");
  b.expect_warnings({{"Warning", 1831, "Duplicate index 'a_2' defined on the table 'pt.u28'. This is deprecated and will be disallowed in a future release."}});
  run(b.s, "CREATE TABLE w33 (a INT, b INT, KEY (a), KEY (a, b), UNIQUE (a), UNIQUE (a))");
  b.expect_warnings({{"Warning", 1831, "Duplicate index 'a_4' defined on the table 'pt.w33'. This is deprecated and will be disallowed in a future release."}});
  run(b.s, "CREATE TABLE x13 (a INT, KEY a (a), b INT UNIQUE, c INT, KEY (a))");
  b.expect_warnings({{"Warning", 1831, "Duplicate index 'a_2' defined on the table 'pt.x13'. This is deprecated and will be disallowed in a future release."}});
  std::vector<std::string> names;
  const auto w33 = b.node.table({"pt", "w33"});
  ASSERT_TRUE(w33);
  for (const auto& k : w33->keys) names.push_back(k.name);
  EXPECT_EQ(names, (std::vector<std::string>{"a", "a_2", "a_3", "a_4"}));
}

std::vector<std::string> column_of(const Outcome& o, size_t column = 0) {
  std::vector<std::string> out;
  for (const auto& r : o.result.value().rows) out.push_back(r.at(column).value_or("NULL"));
  return out;
}

TEST(ExecSessionTables, ShowAndDropTables) {
  TableBed b;
  for (const char* name : {"t1", "T1", "t10", "t2", "t3", "t4"}) run(b.s, std::string("CREATE TABLE ") + name + " (a INT)");
  auto o = run(b.s, "SHOW TABLES");
  ASSERT_EQ(o.result->columns.size(), 1u);
  EXPECT_EQ(o.result->columns[0].name, "Tables_in_pt");
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::var_string, ai, 256, 0, nn | binflag | nodef}}));
  EXPECT_EQ(column_of(o), (std::vector<std::string>{"T1", "t1", "t10", "t2", "t3", "t4"}));
  o = run(b.s, "SHOW TABLES LIKE 't1%'");
  EXPECT_EQ(o.result->columns[0].name, "Tables_in_pt (t1%)");
  EXPECT_EQ(column_of(o), (std::vector<std::string>{"t1", "t10"}));
  o = run(b.s, "SHOW FULL TABLES");
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::var_string, ai, 256, 0, nn | binflag | nodef}, {f::string, ai, 44, 0, nn | binflag | c::enum_ | nodef}}));
  EXPECT_EQ(o.result->columns[1].name, "Table_type");
  EXPECT_EQ(column_of(o, 1), std::vector<std::string>(6, "BASE TABLE"));

  o = run(b.s, "DROP TABLE t1");
  EXPECT_EQ(o.affected_rows, 0u);
  b.expect_refused("DROP TABLE t1", 1051, "42S02", "Unknown table 'pt.t1'");
  run(b.s, "DROP TABLE IF EXISTS t1");
  b.expect_warnings({{"Note", 1051, "Unknown table 'pt.t1'"}});
  // All or none.
  b.expect_refused("DROP TABLE t2, nothere, t3", 1051, "42S02", "Unknown table 'pt.nothere'");
  EXPECT_EQ(column_of(run(b.s, "SHOW TABLES")), (std::vector<std::string>{"T1", "t10", "t2", "t3", "t4"}));
  run(b.s, "DROP TABLE IF EXISTS t4, nothere2");
  b.expect_warnings({{"Note", 1051, "Unknown table 'pt.nothere2'"}});
  b.expect_refused("DROP TABLES t5", 1051, "42S02", "Unknown table 'pt.t5'");
  b.expect_refused("DROP TABLE t2, t2", 1066, "42000", "Not unique table/alias: 't2'");
  // Several missing: each named.
  b.expect_refused("DROP TABLE n1, t2, n2", 1051, "42S02", "Unknown table 'pt.n1,pt.n2'");

  // Another database's tables, by name or with FROM.
  run(b.s, "CREATE DATABASE other; CREATE TABLE other.t40 (a INT)");
  o = run(b.s, "SHOW TABLES FROM other");
  EXPECT_EQ(o.result->columns[0].name, "Tables_in_other");
  EXPECT_EQ(column_of(o), (std::vector<std::string>{"t40"}));
  EXPECT_EQ(column_of(run(b.s, "SHOW TABLES IN other LIKE 't4%'")), (std::vector<std::string>{"t40"}));
  b.expect_refused("SHOW TABLES FROM nodb", 1049, "42000", "Unknown database 'nodb'");
  // A missing database's table is an unknown table.
  b.expect_refused("DROP TABLE nodb.t", 1051, "42S02", "Unknown table 'nodb.t'");
  b.expect_refused("DROP TABLE other.t40, other.zz, nodb.q", 1051, "42S02", "Unknown table 'other.zz,nodb.q'");
  // DROP DATABASE counts the tables it drops.
  run(b.s, "CREATE TABLE other.t41 (a INT)");
  EXPECT_EQ(run(b.s, "DROP DATABASE other").affected_rows, 2u);
  run(b.s, "DROP DATABASE pt");
  b.expect_refused("CREATE TABLE t41 (a INT)", 1046, "3D000", "No database selected");
  b.expect_refused("SHOW TABLES", 1046, "3D000", "No database selected");
  b.expect_refused("DROP TABLE t", 1046, "3D000", "No database selected");
  b.expect_refused("DROP TABLE IF EXISTS t", 1046, "3D000", "No database selected");
}

// SELECT over a table: its columns by name, qualified or not, or *; no rows
// until INSERT.
TEST(ExecSessionTables, SelectNamesColumns) {
  TableBed b;
  run(b.s, "CREATE TABLE t6 (a INT, b INT, PRIMARY KEY (a, b), UNIQUE KEY u (b), KEY k (a), INDEX (b))");
  b.expect_refused("SELECT * FROM nothere", 1146, "42S02", "Table 'pt.nothere' doesn't exist");
  auto o = run(b.s, "SELECT a FROM t6");
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::long_, bin, 11, 0, nn | pri | mul | nodef | num | part}}));
  EXPECT_TRUE(o.result->rows.empty());
  b.expect_refused("SELECT zz FROM t6", 1054, "42S22", "Unknown column 'zz' in 'field list'");
  o = run(b.s, "SELECT t6.a, pt.t6.b, A FROM t6");
  EXPECT_EQ(o.result->columns.size(), 3u);
  EXPECT_EQ(o.result->columns[2].name, "A");
  EXPECT_EQ(o.result->columns[2].original_name, "a");
  o = run(b.s, "SELECT x.a AS y, b FROM t6 AS x");
  EXPECT_EQ(o.result->columns[0].name, "y");
  EXPECT_EQ(o.result->columns[0].original_name, "a");
  EXPECT_EQ(o.result->columns[0].table, "x");
  EXPECT_EQ(o.result->columns[0].original_table, "t6");
  b.expect_refused("SELECT t6.a FROM t6 AS x", 1054, "42S22", "Unknown column 't6.a' in 'field list'");
  o = run(b.s, "SELECT x.*, 1 FROM t6 x");
  EXPECT_EQ(o.result->columns.size(), 3u);
  EXPECT_EQ(o.result->columns[2].schema, "");
  o = run(b.s, "SELECT a, b FROM t6 WHERE a = 1 ORDER BY b LIMIT 3");
  EXPECT_TRUE(o.result->rows.empty());
  b.expect_refused("SELECT a FROM t6 WHERE zz = 1", 1054, "42S22", "Unknown column 'zz' in 'where clause'");
  b.expect_refused("SELECT a FROM t6 ORDER BY zz", 1054, "42S22", "Unknown column 'zz' in 'order clause'");
}

using Rows = std::vector<std::vector<std::string>>;

Rows rows_of(const Outcome& o) {
  Rows out;
  for (const auto& r : o.result.value().rows) {
    out.emplace_back();
    for (const auto& v : r) out.back().push_back(v.value_or("NULL"));
  }
  return out;
}

// INSERT, observed on MySQL 8.4.13 (2026-11-16): values stored as columns
// store them, DEFAULT, AUTO_INCREMENT (a statement reserves a value for each
// of its rows when it first needs one, and those of a failed statement are
// lost), and MySQL's refusals, a failed statement leaving nothing.
TEST(ExecSessionRows, Insert) {
  TableBed b;
  run(b.s, "CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, a INT, b VARCHAR(3) DEFAULT 'x', c DECIMAL(5,2), d INT NOT NULL)");
  auto o = run(b.s, "INSERT INTO t (a, d) VALUES (1, 10)");
  EXPECT_FALSE(o.result);
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.last_insert_id, 1u);
  EXPECT_EQ(o.info, "");
  EXPECT_EQ(row(run(b.s, "SELECT LAST_INSERT_ID(), ROW_COUNT()")), (std::vector<std::string>{"1", "1"}));
  o = run(b.s, "INSERT INTO t (a, d) VALUES (2, 20), (3, 30)");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(o.last_insert_id, 2u);
  EXPECT_EQ(o.info, "Records: 2  Duplicates: 0  Warnings: 0");
  EXPECT_EQ(row(run(b.s, "SELECT LAST_INSERT_ID(), ROW_COUNT()")), (std::vector<std::string>{"2", "2"}));
  run(b.s, "INSERT INTO t VALUES (10, 4, 'abc', 1.5, 40)");
  run(b.s, "INSERT INTO t VALUES (NULL, 5, 'y', 2, 50)");
  run(b.s, "INSERT t SET a = 6, d = 60");
  b.expect_refused("INSERT INTO t (d) VALUES (DEFAULT)", 1364, "HY000", "Field 'd' doesn't have a default value");
  b.expect_refused("INSERT INTO t (a) VALUES (7)", 1364, "HY000", "Field 'd' doesn't have a default value");
  b.expect_refused("INSERT INTO t () VALUES ()", 1364, "HY000", "Field 'd' doesn't have a default value");
  b.expect_refused("INSERT INTO t (a, d) VALUES (8)", 1136, "21S01", "Column count doesn't match value count at row 1");
  b.expect_refused("INSERT INTO t VALUES (1)", 1136, "21S01", "Column count doesn't match value count at row 1");
  b.expect_refused("INSERT INTO t (zz) VALUES (1)", 1054, "42S22", "Unknown column 'zz' in 'field list'");
  b.expect_refused("INSERT INTO t (a, a, d) VALUES (1, 1, 1)", 1110, "42000", "Column 'a' specified twice");
  b.expect_refused("INSERT INTO t (id, d) VALUES (1, 1)", 1062, "23000", "Duplicate entry '1' for key 't.PRIMARY'");
  b.expect_refused("INSERT INTO t (b, d) VALUES ('abcd', 1)", 1406, "22001", "Data too long for column 'b' at row 1");
  b.expect_refused("INSERT INTO t (a, d) VALUES (99999999999, 1)", 1264, "22003", "Out of range value for column 'a' at row 1");
  b.expect_refused("INSERT INTO t (a, d) VALUES ('x', 1)", 1366, "HY000", "Incorrect integer value: 'x' for column 'a' at row 1");
  b.expect_refused("INSERT INTO t (a, d) VALUES ('12x', 1)", 1265, "01000", "Data truncated for column 'a' at row 1");
  run(b.s, "INSERT INTO t (c, d) VALUES (1.234, 1)");
  b.expect_warnings({{"Note", 1265, "Data truncated for column 'c' at row 1"}});
  run(b.s, "INSERT INTO t (b, d) VALUES ('ab ', 1)");
  run(b.s, "INSERT INTO t (b, d) VALUES ('abc  ', 1)");
  b.expect_warnings({{"Note", 1265, "Data truncated for column 'b' at row 1"}});
  b.expect_refused("INSERT INTO t (d) VALUES (NULL)", 1048, "23000", "Column 'd' cannot be null");
  b.expect_refused("INSERT INTO t (a, d) VALUES (1, 2), (2, NULL)", 1048, "23000", "Column 'd' cannot be null");
  b.expect_refused("INSERT INTO t (a, d) VALUES (100, 1), (101, 'q')", 1366, "HY000", "Incorrect integer value: 'q' for column 'd' at row 2");
  // 0 is no value: the next one is taken, past the four lost above.
  o = run(b.s, "INSERT INTO t (id, d) VALUES (0, 1)");
  EXPECT_EQ(o.last_insert_id, 20u);
  b.expect_refused("INSERT INTO nothere VALUES (1)", 1146, "42S02", "Table 'pt.nothere' doesn't exist");
  b.expect_refused("INSERT INTO kv_none.t VALUES (1)", 1049, "42000", "Unknown database 'kv_none'");

  o = run(b.s, "SELECT * FROM t");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "1", "x", "NULL", "10"},
                              {"2", "2", "x", "NULL", "20"},
                              {"3", "3", "x", "NULL", "30"},
                              {"10", "4", "abc", "1.50", "40"},
                              {"11", "5", "y", "2.00", "50"},
                              {"12", "6", "x", "NULL", "60"},
                              {"13", "NULL", "x", "1.23", "1"},
                              {"14", "NULL", "ab ", "NULL", "1"},
                              {"15", "NULL", "abc", "NULL", "1"},
                              {"20", "NULL", "x", "NULL", "1"}}));
  o = run(b.s, "SELECT * FROM t WHERE a > 3 ORDER BY id DESC LIMIT 2");
  EXPECT_EQ(rows_of(o), (Rows{{"12", "6", "x", "NULL", "60"}, {"11", "5", "y", "2.00", "50"}}));
  // Expressions over the columns, and a string compared under its collation.
  o = run(b.s, "SELECT id, a + 1, b, c * 2 FROM t WHERE b = 'X' AND id < 14");
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::long_, bin, 11, 0, nn | pri | autoinc | num | part},
                                           {f::longlong, bin, 12, 0, binflag | num},
                                           {f::var_string, ai, 12, 0, 0},
                                           {f::newdecimal, bin, 8, 2, binflag | num}}));
  EXPECT_EQ(rows_of(o),
            (Rows{{"1", "2", "x", "NULL"}, {"2", "3", "x", "NULL"}, {"3", "4", "x", "NULL"}, {"12", "7", "x", "NULL"}, {"13", "NULL", "x", "2.46"}}));
}

// What each type keeps, and how it is sent.
TEST(ExecSessionRows, StoredValues) {
  TableBed b;
  Outcome o;
  run(b.s, "CREATE TABLE u (a INT, b CHAR(3), c VARBINARY(4), d FLOAT, e DOUBLE, f BIGINT UNSIGNED, g TEXT, h BINARY(3))");
  run(b.s, "INSERT INTO u VALUES (1, 'ab ', 'x', 1.1, 1.1, 18446744073709551615, 'long text', 'a')");
  run(b.s, "INSERT INTO u (a, d) VALUES (3.5e0, 3.4e38), (-2.5, 1e-10), (2.5, NULL)");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM u")), (Rows{{"1", "ab", "x", "1.1", "1.1", "18446744073709551615", "long text", std::string("a\0\0", 3)},
                                                        {"4", "NULL", "NULL", "3.4e38", "NULL", "NULL", "NULL", "NULL"},
                                                        {"-3", "NULL", "NULL", "0.0000000001", "NULL", "NULL", "NULL", "NULL"},
                                                        {"3", "NULL", "NULL", "NULL", "NULL", "NULL", "NULL", "NULL"}}));
  // A FLOAT computes as the float it holds.
  EXPECT_EQ(row(run(b.s, "SELECT d * 1 FROM u WHERE a = 1")), (std::vector<std::string>{"1.100000023841858"}));
  run(b.s, "CREATE TABLE z (a INT(5) ZEROFILL, b DECIMAL(6,2) ZEROFILL, c DOUBLE, e INT UNIQUE)");
  run(b.s, "INSERT INTO z VALUES (42, 3.5, -0.0, 1)");
  o = run(b.s, "SELECT *, a + 0 FROM z");
  EXPECT_EQ(rows_of(o), (Rows{{"00042", "0003.50", "0", "1", "42"}}));
  // Arithmetic counts INT's own digits, not the display width (found by the
  // differential gate).
  EXPECT_EQ(o.result->columns[4].type.length, 11u);
  b.expect_refused("INSERT INTO z (e) VALUES (1)", 1062, "23000", "Duplicate entry '1' for key 'z.e'");
  run(b.s, "INSERT INTO z (e) VALUES (NULL), (NULL)");
  EXPECT_EQ(rows_of(run(b.s, "SELECT e FROM z ORDER BY e")), (Rows{{"NULL"}, {"NULL"}, {"1"}}));
  b.expect_refused("INSERT INTO z (b) VALUES ('x')", 1366, "HY000", "Incorrect decimal value: 'x' for column 'b' at row 1");
  b.expect_refused("INSERT INTO z (b) VALUES ('1.5x')", 1366, "HY000", "Incorrect decimal value: '1.5x' for column 'b' at row 1");
  b.expect_refused("INSERT INTO z (c) VALUES ('x')", 1265, "01000", "Data truncated for column 'c' at row 1");
  b.expect_refused("INSERT INTO z (b) VALUES (99999)", 1264, "22003", "Out of range value for column 'b' at row 1");
  run(b.s, "CREATE TABLE m (b VARCHAR(5) CHARACTER SET utf8mb3)");
  b.expect_refused("INSERT INTO m VALUES (X'F09F9880')", 1366, "HY000", "Incorrect string value: '\\xF0\\x9F\\x98\\x80' for column 'b' at row 1");
}

// Order: a table with no primary key keeps the order rows came in; ORDER BY
// puts NULL first ascending and last descending; a string key orders and
// collides under its collation, which pads no spaces.
TEST(ExecSessionRows, Order) {
  TableBed b;
  run(b.s, "CREATE TABLE n (a INT, b VARCHAR(5))");
  run(b.s, "INSERT INTO n VALUES (3, 'c'), (1, 'a'), (NULL, 'n'), (2, 'b')");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM n")), (Rows{{"3", "c"}, {"1", "a"}, {"NULL", "n"}, {"2", "b"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM n ORDER BY a")), (Rows{{"NULL", "n"}, {"1", "a"}, {"2", "b"}, {"3", "c"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM n ORDER BY a DESC")), (Rows{{"3", "c"}, {"2", "b"}, {"1", "a"}, {"NULL", "n"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT b FROM n ORDER BY b DESC LIMIT 1, 2")), (Rows{{"c"}, {"b"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT b AS x FROM n ORDER BY x LIMIT 1")), (Rows{{"a"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT b, a FROM n ORDER BY 2 DESC LIMIT 1")), (Rows{{"c", "3"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT a, b FROM n WHERE b LIKE 'a%' OR a IS NULL")), (Rows{{"1", "a"}, {"NULL", "n"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM n AS x WHERE x.a = 1")), (Rows{{"1", "a"}}));
  run(b.s, "CREATE TABLE k (s VARCHAR(5) PRIMARY KEY, v INT)");
  run(b.s, "INSERT INTO k VALUES ('b', 1), ('a', 2), ('C', 3)");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM k")), (Rows{{"a", "2"}, {"b", "1"}, {"C", "3"}}));
  b.expect_refused("INSERT INTO k VALUES ('A', 4)", 1062, "23000", "Duplicate entry 'A' for key 'k.PRIMARY'");
  run(b.s, "INSERT INTO k VALUES ('a ', 4)");
  EXPECT_EQ(run(b.s, "SELECT * FROM k").result->rows.size(), 4u);
}

// The manual's "Consistent Nonlocking Reads" across two sessions of one
// node: what a transaction writes no one else sees until it commits; a
// REPEATABLE READ transaction reads the snapshot of its first read;
// ROLLBACK leaves nothing; a failed statement undoes only itself; DDL
// commits first ("Statements That Cause an Implicit Commit").
TEST(ExecSessionRows, Transactions) {
  TableBed b;
  auto other = b.other();
  run(b.s, "CREATE TABLE t (a INT PRIMARY KEY)");
  run(b.s, "BEGIN");
  run(b.s, "INSERT INTO t VALUES (1)");
  EXPECT_EQ(rows_of(run(b.s, "SELECT a FROM t")), (Rows{{"1"}}));
  EXPECT_TRUE(rows_of(run(other, "SELECT a FROM t")).empty());
  run(b.s, "COMMIT");
  EXPECT_EQ(rows_of(run(other, "SELECT a FROM t")), (Rows{{"1"}}));

  run(other, "START TRANSACTION");
  EXPECT_EQ(rows_of(run(other, "SELECT a FROM t")), (Rows{{"1"}}));
  run(b.s, "INSERT INTO t VALUES (2)");
  EXPECT_EQ(rows_of(run(other, "SELECT a FROM t")), (Rows{{"1"}}));
  run(other, "COMMIT");
  EXPECT_EQ(rows_of(run(other, "SELECT a FROM t")), (Rows{{"1"}, {"2"}}));

  // READ COMMITTED: each statement sees what has committed.
  run(other, "SET transaction_isolation = 'READ-COMMITTED'");
  run(other, "BEGIN");
  EXPECT_EQ(run(other, "SELECT a FROM t").result->rows.size(), 2u);
  run(b.s, "INSERT INTO t VALUES (3)");
  EXPECT_EQ(run(other, "SELECT a FROM t").result->rows.size(), 3u);
  run(other, "COMMIT");

  run(b.s, "BEGIN");
  run(b.s, "INSERT INTO t VALUES (4)");
  b.expect_refused("INSERT INTO t VALUES (5), (4)", 1062, "23000", "Duplicate entry '4' for key 't.PRIMARY'");
  EXPECT_EQ(run(b.s, "SELECT a FROM t").result->rows.size(), 4u);
  run(b.s, "ROLLBACK");
  EXPECT_EQ(run(b.s, "SELECT a FROM t").result->rows.size(), 3u);

  // autocommit = 0 opens a transaction with the first statement.
  run(b.s, "SET autocommit = 0");
  run(b.s, "INSERT INTO t VALUES (6)");
  EXPECT_TRUE(b.s.in_transaction());
  EXPECT_EQ(run(other, "SELECT a FROM t").result->rows.size(), 3u);
  // DDL commits what came before it.
  run(b.s, "CREATE TABLE t2 (a INT)");
  EXPECT_EQ(run(other, "SELECT a FROM t").result->rows.size(), 4u);
  run(b.s, "SET autocommit = 1");

  // A row another transaction has written and not committed: the statement
  // must wait for its lock (the command phase waits; see
  // server_commands_test), and leaves nothing behind meanwhile.
  run(b.s, "BEGIN");
  run(b.s, "INSERT INTO t VALUES (7)");
  EXPECT_THROW(run(other, "INSERT INTO t VALUES (7)"), LockWaitNeeded);
  EXPECT_FALSE(other.in_transaction());
  run(b.s, "COMMIT");
  EXPECT_EQ(refusal(other, "INSERT INTO t VALUES (7)").code, 1062);

  // A dropped table's rows go with it.
  run(b.s, "DROP TABLE t");
  run(b.s, "CREATE TABLE t (a INT PRIMARY KEY)");
  EXPECT_TRUE(run(b.s, "SELECT a FROM t").result->rows.empty());

  // SERIALIZABLE needs read marks Kvasir does not keep yet.
  run(b.s, "SET transaction_isolation = 'SERIALIZABLE'");
  b.expect_refused("SELECT a FROM t", 1235, "42000", "This version of Kvasir doesn't yet support 'SERIALIZABLE'");
}

TEST(ExecSessionRows, InsertsKvasirDoesNotYetCarryOut) {
  TableBed b;
  run(b.s, "CREATE TABLE n (a INT, b INT)");
  b.expect_refused("INSERT IGNORE INTO n VALUES (1, 2)", 1235, "42000", "This version of Kvasir doesn't yet support 'INSERT IGNORE'");
  b.expect_refused("REPLACE INTO n VALUES (1, 2)", 1235, "42000", "This version of Kvasir doesn't yet support 'REPLACE'");
  b.expect_refused("INSERT INTO n (a, b) VALUES (5, a)", 1235, "42000", "This version of Kvasir doesn't yet support 'columns in VALUES'");
}

}  // namespace
}  // namespace kvasir::exec
