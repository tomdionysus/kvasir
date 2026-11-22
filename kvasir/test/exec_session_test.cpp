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
  static const VariableRegistry r("ephemeral", "8.4.13-kvasir", "Kvasir - The Distributed Relational Database (in memory, not durable)");
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

// Observed on MySQL 8.4.13 (Oracle's container, 2026-11-19): the `mysql`
// client's `status` command reads @@character_set_database. Both variables
// follow USE, are the server's with no database, and DEFAULT is the
// database's pair; setting either warns it will become read-only, and
// carries the other with it.
TEST(ExecSession, TheDatabaseCharacterSet) {
  catalog::Catalog cat;
  txn::Store rows;
  auto s = make(cat, rows);
  using V = std::vector<std::string>;
  const auto pair = [&] { return row(run(s, "SELECT @@character_set_database, @@collation_database")); };
  const Warning collation_warning{"Warning", 1681, "Updating 'collation_database' is deprecated. It will be made read-only in a future release."};
  const Warning charset_warning{"Warning", 1681, "Updating 'character_set_database' is deprecated. It will be made read-only in a future release."};
  const auto warned = [&](const std::vector<Warning>& expected) {
    ASSERT_EQ(s.warnings().size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(s.warnings()[i].level, expected[i].level) << i;
      EXPECT_EQ(s.warnings()[i].code, expected[i].code) << i;
      EXPECT_EQ(s.warnings()[i].message, expected[i].message) << i;
    }
  };

  EXPECT_EQ(pair(), (V{"utf8mb4", "utf8mb4_0900_ai_ci"}));
  EXPECT_EQ(row(run(s, "SELECT @@global.character_set_database, @@session.collation_database")), (V{"utf8mb4", "utf8mb4_0900_ai_ci"}));
  const auto o = run(s, "SELECT @@character_set_database");
  EXPECT_EQ(o.result->columns[0].type.field_type, f::var_string);
  EXPECT_EQ(o.result->columns[0].type.length, 87380u);
  EXPECT_EQ(o.result->columns[0].type.collation, 255);

  run(s, "CREATE DATABASE kv_dbcs COLLATE utf8mb4_bin");
  run(s, "CREATE DATABASE kv_dbcs2 CHARACTER SET binary");
  run(s, "USE kv_dbcs2");
  EXPECT_EQ(pair(), (V{"binary", "binary"}));
  run(s, "USE kv_dbcs");
  EXPECT_EQ(pair(), (V{"utf8mb4", "utf8mb4_bin"}));

  run(s, "SET collation_database = 'utf8mb4_0900_as_cs'");
  warned({collation_warning});
  EXPECT_EQ(pair(), (V{"utf8mb4", "utf8mb4_0900_as_cs"}));
  run(s, "SET character_set_database = 'binary'");
  warned({charset_warning});
  EXPECT_EQ(pair(), (V{"binary", "binary"}));
  run(s, "SET collation_database = DEFAULT");
  warned({collation_warning});
  EXPECT_EQ(pair(), (V{"utf8mb4", "utf8mb4_bin"}));
  run(s, "SET character_set_database = 'UTF8MB4'");
  EXPECT_EQ(pair(), (V{"utf8mb4", "utf8mb4_0900_ai_ci"}));
  run(s, "SET collation_database = 'utf8mb4_general_ci', character_set_database = 'binary'");
  warned({collation_warning, charset_warning});
  EXPECT_EQ(pair(), (V{"binary", "binary"}));
  run(s, "SET character_set_database = DEFAULT");
  EXPECT_EQ(pair(), (V{"utf8mb4", "utf8mb4_bin"}));

  auto r = refusal(s, "SET character_set_database = 'nope'");
  EXPECT_EQ(r.code, 1115);
  EXPECT_EQ(r.message, "Unknown character set: 'nope'");
  r = refusal(s, "SET collation_database = 'utf8mb4_nope'");
  EXPECT_EQ(r.code, 1273);
  EXPECT_EQ(r.message, "Unknown collation: 'utf8mb4_nope'");
  r = refusal(s, "SET collation_database = NULL");
  EXPECT_EQ(r.code, 1231);
  EXPECT_EQ(r.message, "Variable 'collation_database' can't be set to the value of 'NULL'");
  EXPECT_EQ(refusal(s, "SET character_set_database = NULL").message, "Variable 'character_set_database' can't be set to the value of 'NULL'");
  r = refusal(s, "SET collation_connection = NULL");
  EXPECT_EQ(r.code, 1231);
  EXPECT_EQ(r.message, "Variable 'collation_connection' can't be set to the value of 'NULL'");

  // Dropping another database changes nothing; dropping the current one
  // leaves the server's.
  run(s, "DROP DATABASE kv_dbcs2");
  EXPECT_EQ(pair(), (V{"utf8mb4", "utf8mb4_bin"}));
  run(s, "DROP DATABASE kv_dbcs");
  EXPECT_EQ(pair(), (V{"utf8mb4", "utf8mb4_0900_ai_ci"}));
}

// Observed on MySQL 8.4.13 (2026-11-20, Go's driver through a capturing
// proxy): the collation a client asks for in its handshake is the session's
// as SET NAMES <its character set> COLLATE <it> makes it, and results are
// reported in it. Go asks for utf8mb4_general_ci (45) by default. A
// character set Kvasir does not convert (latin1) leaves the server's
// (MySQL takes it: a known difference).
TEST(ExecSession, TheHandshakesCollation) {
  using V = std::vector<std::string>;
  const auto with = [](uint16_t collation) { return Session(registry(), node_catalog(), node_store(), SessionInfo{"root", "10.0.0.1", 7, collation}); };
  const std::string sql = "SELECT @@character_set_client, @@character_set_connection, @@character_set_results, @@collation_connection, 'a'";
  auto s = with(45);
  auto o = run(s, sql);
  EXPECT_EQ(row(o), (V{"utf8mb4", "utf8mb4", "utf8mb4", "utf8mb4_general_ci", "a"}));
  for (const auto& c : o.result->columns) EXPECT_EQ(c.type.collation, 45) << c.name;
  EXPECT_EQ(o.result->columns[0].type.length, 87380u);
  EXPECT_EQ(o.result->columns[4].type.flags, c::not_null);
  // COM_RESET_CONNECTION keeps it.
  run(s, "SET NAMES utf8mb4");
  s.reset();
  EXPECT_EQ(row(run(s, "SELECT @@collation_connection")), (V{"utf8mb4_general_ci"}));

  auto bin = with(46);
  o = run(bin, sql);
  EXPECT_EQ(row(o), (V{"utf8mb4", "utf8mb4", "utf8mb4", "utf8mb4_bin", "a"}));
  for (const auto& c : o.result->columns) EXPECT_EQ(c.type.collation, 46) << c.name;
  EXPECT_EQ(o.result->columns[4].type.flags, c::not_null | c::binary);

  auto binary = with(63);
  EXPECT_EQ(row(run(binary, sql)), (V{"binary", "binary", "binary", "binary", "a"}));

  for (const uint16_t other : {uint16_t{8}, uint16_t{0}, uint16_t{255}}) {
    auto s2 = with(other);
    EXPECT_EQ(row(run(s2, sql)), (V{"utf8mb4", "utf8mb4", "utf8mb4", "utf8mb4_0900_ai_ci", "a"})) << other;
  }
}

// Prepared statements, from what MySQL 8.4.13 answered Go's driver and
// mysql-connector-python (captured through a proxy, 2026-11-20). Flags are
// compared without NUM, which MySQL's server does not send (its client
// library adds it).
struct Typed {
  uint8_t field_type = 0;
  uint16_t collation = 0;
  uint32_t length = 0;
  uint8_t decimals = 0;
  uint16_t flags = 0;
  friend bool operator==(const Typed&, const Typed&) = default;
  friend std::ostream& operator<<(std::ostream& o, const Typed& t) {
    return o << int{t.field_type} << "/" << t.collation << "/" << t.length << "/" << int{t.decimals} << "/" << t.flags;
  }
};

std::vector<Typed> typed(const std::vector<Column>& columns) {
  std::vector<Typed> out;
  for (const auto& col : columns)
    out.push_back({col.type.field_type, col.type.collation, col.type.length, col.type.decimals, static_cast<uint16_t>(col.type.flags & ~c::num)});
  return out;
}

Prepared prepare(Session& s, const std::string& sql) {
  auto options = s.parser_options();
  options.placeholders = true;
  return s.prepare(std::move(sql::parse(sql, options).at(0)));
}

protocol::BoundValue bound(uint8_t type, decltype(protocol::BoundValue::value) value, bool is_unsigned = false) {
  return {{type, is_unsigned}, std::move(value)};
}

std::vector<std::optional<std::string>> values(const Outcome& o, size_t i = 0) { return o.result.value().rows.at(i); }

template <typename F>
Refusal refusal_of(F f) {
  try {
    f();
  } catch (const SqlError& e) {
    return {e.code(), e.sqlstate(), e.what()};
  }
  return {};
}

constexpr uint16_t binary_set = 63, ai_ci = 255, bin_flag = c::binary, unsigned_flag = c::unsigned_;
const Typed longlong_param{f::longlong, binary_set, 21, 0, bin_flag};

struct PreparedBed {
  catalog::Catalog node;
  txn::Store rows;
  Session s;
  explicit PreparedBed(uint16_t collation = 255) : s(registry(), node, rows, SessionInfo{"root", "10.0.0.1", 7, collation}) {
    for (const char* sql :
         {"CREATE DATABASE kv_prep", "USE kv_prep", "CREATE TABLE t (a INT PRIMARY KEY, b VARCHAR(10))", "INSERT INTO t VALUES (1, 'x'), (2, 'y')"})
      run(s, sql);
  }
};

TEST(ExecSessionPrepared, ASelectWithParameters) {
  PreparedBed b(45);
  const auto p = prepare(b.s, "SELECT a, b FROM t WHERE a > ? AND b <> ?");
  EXPECT_EQ(p.id, 1u);
  ASSERT_EQ(p.params.size(), 2u);
  EXPECT_EQ(p.params[0].name, "?");
  EXPECT_EQ(typed(p.params), (std::vector<Typed>{longlong_param, {f::var_string, 45, 65532, 31, 0}}));
  // The result as the same text query reports it.
  const auto text = run(b.s, "SELECT a, b FROM t");
  EXPECT_EQ(typed(p.columns), typed(text.result->columns));
  EXPECT_EQ(p.columns[1].schema, "kv_prep");
  EXPECT_EQ(p.columns[1].original_name, "b");

  auto o = b.s.execute_prepared(p.id, b.s.bind(p.id, {bound(f::longlong, int64_t{0}), bound(f::string, std::string("zz"))}));
  ASSERT_EQ(o.result->rows.size(), 2u);
  EXPECT_EQ(values(o, 1), (std::vector<std::optional<std::string>>{"2", "y"}));
  EXPECT_EQ(typed(o.result->columns), typed(text.result->columns));
  o = b.s.execute_prepared(p.id, b.s.bind(p.id, {bound(f::longlong, int64_t{1}), bound(f::string, std::string("x"))}));
  ASSERT_EQ(o.result->rows.size(), 1u);
  EXPECT_EQ(values(o), (std::vector<std::optional<std::string>>{"2", "y"}));
}

TEST(ExecSessionPrepared, EachParameterTypeAsBound) {
  PreparedBed b(45);
  const auto p = prepare(b.s, "SELECT ?, ? + 1, CONCAT(?, 'x'), ?");
  const Typed string45{f::var_string, 45, 65532, 31, 0};
  EXPECT_EQ(typed(p.params), (std::vector<Typed>{string45, longlong_param, string45, string45}));
  EXPECT_EQ(typed(p.columns), (std::vector<Typed>{string45, {f::longlong, binary_set, 22, 0, bin_flag}, {f::medium_blob, 45, 262144, 31, 0}, string45}));
  const auto o = b.s.execute_prepared(
      p.id, b.s.bind(p.id, {bound(f::longlong, int64_t{-5}), bound(f::double_, 2.5), bound(f::string, std::string("ab")), bound(f::null, std::monostate{})}));
  EXPECT_EQ(typed(o.result->columns),
            (std::vector<Typed>{longlong_param, {f::double_, binary_set, 23, 31, bin_flag}, {f::medium_blob, 45, 262144, 31, 0}, string45}));
  EXPECT_EQ(values(o), (std::vector<std::optional<std::string>>{"-5", "3.5", "abx", std::nullopt}));

  // Go: an unsigned BIGINT, a float sent as DOUBLE, bytes, a TINY; python: a
  // DECIMAL.
  PreparedBed c;
  const auto q = prepare(c.s, "SELECT ?, ?, ?, ?, ?");
  const auto r =
      c.s.execute_prepared(q.id, c.s.bind(q.id, {bound(f::longlong, UINT64_MAX, true), bound(f::double_, 1.5), bound(f::string, std::string("\x01\x02")),
                                                 bound(f::tiny, int64_t{1}), bound(f::decimal, std::string("1.25"))}));
  EXPECT_EQ(typed(r.result->columns), (std::vector<Typed>{{f::longlong, binary_set, 21, 0, static_cast<uint16_t>(bin_flag | unsigned_flag)},
                                                          {f::double_, binary_set, 23, 31, bin_flag},
                                                          {f::var_string, ai_ci, 65532, 31, 0},
                                                          longlong_param,
                                                          {f::newdecimal, binary_set, 67, 30, bin_flag}}));
  EXPECT_EQ(values(r), (std::vector<std::optional<std::string>>{"18446744073709551615", "1.5", std::string("\x01\x02"), "1", "1.25"}));
  // Kvasir has no date or time types.
  EXPECT_EQ(refusal_of([&] {
              c.s.bind(q.id, {bound(f::date, std::string("\xe8\x07\x0b\x14", 4)), bound(f::null, std::monostate{}), bound(f::null, std::monostate{}),
                              bound(f::null, std::monostate{}), bound(f::null, std::monostate{})});
            }).code,
            1235);
}

TEST(ExecSessionPrepared, ChangesAndLimits) {
  PreparedBed b;
  auto p = prepare(b.s, "INSERT INTO t VALUES (?, ?)");
  EXPECT_TRUE(p.columns.empty());
  EXPECT_EQ(typed(p.params), (std::vector<Typed>{longlong_param, {f::var_string, ai_ci, 65532, 31, 0}}));
  auto o = b.s.execute_prepared(p.id, b.s.bind(p.id, {bound(f::longlong, int64_t{9}), bound(f::null, std::monostate{})}));
  EXPECT_FALSE(o.result);
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(row(run(b.s, "SELECT a, b FROM t WHERE a = 9")), (std::vector<std::string>{"9", "NULL"}));
  p = prepare(b.s, "DELETE FROM t WHERE a = ?");
  EXPECT_EQ(typed(p.params), (std::vector<Typed>{longlong_param}));
  o = b.s.execute_prepared(p.id, b.s.bind(p.id, {bound(f::longlong, int64_t{9})}));
  EXPECT_EQ(o.affected_rows, 1u);
  // A value set into an integer column is a BIGINT parameter.
  p = prepare(b.s, "UPDATE t SET a = ? WHERE a = 2");
  EXPECT_EQ(typed(p.params), (std::vector<Typed>{longlong_param}));
  o = b.s.execute_prepared(p.id, b.s.bind(p.id, {bound(f::longlong, int64_t{7})}));
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(row(run(b.s, "SELECT a, b FROM t WHERE a = 7")), (std::vector<std::string>{"7", "y"}));

  p = prepare(b.s, "SELECT ? FROM t LIMIT ?");
  EXPECT_EQ(typed(p.params),
            (std::vector<Typed>{{f::var_string, ai_ci, 65532, 31, 0}, {f::longlong, binary_set, 21, 0, static_cast<uint16_t>(bin_flag | unsigned_flag)}}));
  o = b.s.execute_prepared(p.id, b.s.bind(p.id, {bound(f::longlong, int64_t{1}), bound(f::longlong, int64_t{1})}));
  EXPECT_EQ(typed(o.result->columns), (std::vector<Typed>{longlong_param}));
  ASSERT_EQ(o.result->rows.size(), 1u);
  EXPECT_EQ(values(o), (std::vector<std::optional<std::string>>{"1"}));
}

// A LIMIT or OFFSET placeholder as MySQL 8.4.13 reads what COM_STMT_EXECUTE
// binds to it (observed through mysql-connector-python's binary protocol): a
// negative integer is 1690, a DOUBLE or DECIMAL 1210, NULL is 0, and a
// string is read as strtoull reads it, its leading digits, a minus sign
// wrapping and past 2^64 the largest; no warnings.
TEST(ExecSessionPrepared, LimitParameters) {
  PreparedBed b;
  const auto p = prepare(b.s, "SELECT a FROM t ORDER BY a LIMIT ?");
  const auto rows = [&](protocol::BoundValue v) { return b.s.execute_prepared(p.id, b.s.bind(p.id, {std::move(v)})).result->rows.size(); };
  EXPECT_EQ(rows(bound(f::longlong, int64_t{1})), 1u);
  EXPECT_EQ(rows(bound(f::longlong, uint64_t{18446744073709551615u}, true)), 2u);
  EXPECT_EQ(rows(bound(f::null, std::monostate{})), 0u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("1"))), 1u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("1.9"))), 1u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("2x"))), 2u);
  EXPECT_EQ(rows(bound(f::var_string, std::string(" +1"))), 1u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("1e1"))), 1u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("0x2"))), 0u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("x"))), 0u);
  EXPECT_EQ(rows(bound(f::var_string, std::string(""))), 0u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("-1"))), 2u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("-0"))), 0u);
  EXPECT_EQ(rows(bound(f::var_string, std::string("99999999999999999999"))), 2u);
  EXPECT_TRUE(b.s.warnings().empty());

  auto r = refusal_of([&] { rows(bound(f::longlong, int64_t{-1})); });
  EXPECT_EQ(r.code, 1690);
  EXPECT_EQ(r.sqlstate, "22003");
  EXPECT_EQ(r.message, "signed integer value is out of range in 'mysqld_stmt_execute'");
  for (auto v : {bound(f::double_, 1.5), bound(f::double_, 2.0), bound(f::newdecimal, std::string("1"))}) {
    r = refusal_of([&] { rows(v); });
    EXPECT_EQ(r.code, 1210);
    EXPECT_EQ(r.sqlstate, "HY000");
    EXPECT_EQ(r.message, "Incorrect arguments to mysqld_stmt_execute");
  }

  // The offset alike: a string's leading digits, -1 past every row.
  const auto q = prepare(b.s, "SELECT a FROM t ORDER BY a LIMIT ?, 5");
  const auto from = [&](protocol::BoundValue v) { return b.s.execute_prepared(q.id, b.s.bind(q.id, {std::move(v)})).result->rows.size(); };
  EXPECT_EQ(from(bound(f::var_string, std::string("1"))), 1u);
  EXPECT_EQ(from(bound(f::var_string, std::string("-1"))), 0u);
  EXPECT_EQ(from(bound(f::null, std::monostate{})), 2u);
  EXPECT_EQ(refusal_of([&] { from(bound(f::longlong, int64_t{-5})); }).code, 1690);
}

TEST(ExecSessionPrepared, WhatIsRefused) {
  PreparedBed b;
  // A table that is not there, at the prepare.
  auto r = refusal_of([&] { prepare(b.s, "SELECT * FROM nope WHERE a = ?"); });
  EXPECT_EQ(r.code, 1146);
  EXPECT_EQ(r.message, "Table 'kv_prep.nope' doesn't exist");
  r = refusal_of([&] { prepare(b.s, "INSERT INTO nope VALUES (?)"); });
  EXPECT_EQ(r.code, 1146);
  // A column that is not there, at the prepare (observed).
  r = refusal_of([&] { prepare(b.s, "UPDATE t SET nope = ?"); });
  EXPECT_EQ(r.code, 1054);
  EXPECT_EQ(r.sqlstate, "42S22");
  EXPECT_EQ(r.message, "Unknown column 'nope' in 'field list'");
  // What the text query refuses as not built, the prepare refuses alike.
  r = refusal_of([&] { prepare(b.s, "INSERT INTO t SELECT ?, ?"); });
  EXPECT_EQ(r.code, 1235);
  EXPECT_EQ(r.message, "This version of Kvasir doesn't yet support 'INSERT ... SELECT'");
  // An id not prepared, or closed.
  r = refusal_of([&] { b.s.execute_prepared(5, {}); });
  EXPECT_EQ(r.code, 1243);
  EXPECT_EQ(r.sqlstate, "HY000");
  EXPECT_EQ(r.message, "Unknown prepared statement handler (5) given to mysqld_stmt_execute");
  const auto p = prepare(b.s, "SELECT 1");
  EXPECT_EQ(b.s.parameter_types(p.id, "mysqld_stmt_execute").size(), 0u);
  b.s.close_prepared(p.id);
  b.s.close_prepared(p.id);
  EXPECT_EQ(refusal_of([&] { b.s.parameter_types(p.id, "mysqld_stmt_reset"); }).message,
            "Unknown prepared statement handler (" + std::to_string(p.id) + ") given to mysqld_stmt_reset");
  // COM_RESET_CONNECTION forgets them.
  const auto q = prepare(b.s, "SELECT 1");
  b.s.reset();
  EXPECT_EQ(refusal_of([&] { b.s.execute_prepared(q.id, {}); }).code, 1243);
  // At most max_prepared_statements.
  PreparedBed many;
  for (size_t i = 0; i < max_prepared_statements; ++i) prepare(many.s, "SELECT 1");
  r = refusal_of([&] { prepare(many.s, "SELECT 1"); });
  EXPECT_EQ(r.code, 1461);
  EXPECT_EQ(r.sqlstate, "42000");
  EXPECT_EQ(r.message, "Can't create more than max_prepared_stmt_count statements (current value: 16382)");
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

// The manual's examples (String Comparison Functions, STRCMP): -1, 0 or 1
// by the collation the two arguments aggregate to, NULL for a NULL, and 1267
// for collations that cannot be reconciled. Connector/C++'s getTables uses it.
TEST(ExecSession, Strcmp) {
  auto s = make();
  EXPECT_EQ(row(run(s, "SELECT STRCMP('text', 'text2'), STRCMP('text2', 'text'), STRCMP('text', 'text'), STRCMP(NULL, 'a'), STRCMP('a', NULL)")),
            (std::vector<std::string>{"-1", "1", "0", "NULL", "NULL"}));
  run(s,
      "SET @s1 = _utf8mb4 'x' COLLATE utf8mb4_0900_ai_ci, @s2 = _utf8mb4 'X' COLLATE utf8mb4_0900_ai_ci, "
      "@s3 = _utf8mb4 'x' COLLATE utf8mb4_0900_as_cs, @s4 = _utf8mb4 'X' COLLATE utf8mb4_0900_as_cs");
  EXPECT_EQ(row(run(s, "SELECT STRCMP(@s1, @s2), STRCMP(@s3, @s4), STRCMP(@s1, @s3 COLLATE utf8mb4_0900_ai_ci)")), (std::vector<std::string>{"0", "-1", "0"}));
  const auto mixed = refusal(s, "SELECT STRCMP(@s1, @s3)");
  EXPECT_EQ(mixed.code, 1267);
  EXPECT_EQ(mixed.message, "Illegal mix of collations (utf8mb4_0900_ai_ci,IMPLICIT) and (utf8mb4_0900_as_cs,IMPLICIT) for operation 'strcmp'");
  EXPECT_EQ(refusal(s, "SELECT STRCMP('a')").code, 1582);
}

TEST(ExecSession, ColumnNames) {
  auto s = make();
  const auto o = run(s, "SELECT 1+1, 'abc', 1 AS one, @@version_comment, @a");
  std::vector<std::string> names;
  for (const auto& c : o.result->columns) names.push_back(c.name);
  EXPECT_EQ(names, (std::vector<std::string>{"1+1", "abc", "one", "@@version_comment", "@a"}));
}

// A generated name is cut to at most 255 bytes, a character whole, with no
// warning (observed on MySQL 8.4.13: 300 a's give 255, 200 é's 127 of them).
TEST(ExecSession, AGeneratedColumnNameIsCutTo255Bytes) {
  auto s = make();
  std::string e;
  for (int i = 0; i < 200; ++i) e += "é";
  const auto o = run(s, "SELECT LENGTH('" + std::string(300, 'a') + "'), '" + e + "'");
  EXPECT_EQ(o.result->columns[0].name, "LENGTH('" + std::string(247, 'a'));
  std::string cut;
  for (int i = 0; i < 127; ++i) cut += "é";
  EXPECT_EQ(o.result->columns[1].name, cut);
  EXPECT_EQ(row(o), (std::vector<std::string>{"300", e}));
  EXPECT_TRUE(s.warnings().empty());
}

// Under DISTINCT, an ORDER BY expression that is not a select item may name
// only the list's columns: else 3065, naming the expression's position and
// the first other column (observed on MySQL 8.4.13).
TEST(ExecSession, DistinctOrdersOnlyByWhatItLists) {
  catalog::Catalog node;
  txn::Store rows;
  auto s = make(node, rows);
  run(s, "CREATE DATABASE kv_d");
  run(s, "CREATE TABLE kv_d.t (a INT PRIMARY KEY, b INT, c INT)");
  run(s, "INSERT INTO kv_d.t VALUES (1, 2, 3), (2, 2, 1)");
  const auto message = [](int n, const std::string& column) {
    return "Expression #" + std::to_string(n) + " of ORDER BY clause is not in SELECT list, references column '" + column +
           "' which is not in SELECT list; this is incompatible with DISTINCT";
  };
  auto r = refusal(s, "SELECT DISTINCT b FROM kv_d.t ORDER BY c");
  EXPECT_EQ(r.code, 3065);
  EXPECT_EQ(r.sqlstate, "HY000");
  EXPECT_EQ(r.message, message(1, "kv_d.t.c"));
  EXPECT_EQ(refusal(s, "SELECT DISTINCT b FROM kv_d.t ORDER BY a").message, message(1, "kv_d.t.a"));
  EXPECT_EQ(refusal(s, "SELECT DISTINCT b FROM kv_d.t ORDER BY b, c").message, message(2, "kv_d.t.c"));
  EXPECT_EQ(refusal(s, "SELECT DISTINCT b FROM kv_d.t ORDER BY c + b").message, message(1, "kv_d.t.c"));
  EXPECT_EQ(refusal(s, "SELECT DISTINCT b FROM kv_d.t ORDER BY ABS(c)").message, message(1, "kv_d.t.c"));
  EXPECT_EQ(refusal(s, "SELECT DISTINCT b FROM kv_d.t AS u ORDER BY c DESC").message, message(1, "kv_d.u.c"));
  EXPECT_EQ(refusal(s, "SELECT DISTINCT b + c FROM kv_d.t ORDER BY c").message, message(1, "kv_d.t.c"));

  EXPECT_EQ(row(run(s, "SELECT DISTINCT b FROM kv_d.t ORDER BY b + 1")), (std::vector<std::string>{"2"}));
  EXPECT_EQ(row(run(s, "SELECT DISTINCT b AS x FROM kv_d.t ORDER BY x")), (std::vector<std::string>{"2"}));
  EXPECT_EQ(row(run(s, "SELECT DISTINCT b FROM kv_d.t ORDER BY 1")), (std::vector<std::string>{"2"}));
  EXPECT_EQ(row(run(s, "SELECT DISTINCT b FROM kv_d.t ORDER BY t.b")), (std::vector<std::string>{"2"}));
  EXPECT_EQ(row(run(s, "SELECT DISTINCT b + c FROM kv_d.t ORDER BY b + c")), (std::vector<std::string>{"3"}));
  EXPECT_EQ(row(run(s, "SELECT DISTINCT b, c FROM kv_d.t ORDER BY c")), (std::vector<std::string>{"2", "1"}));
  EXPECT_EQ(row(run(s, "SELECT DISTINCT * FROM kv_d.t ORDER BY c")), (std::vector<std::string>{"2", "2", "1"}));
  // Without DISTINCT, any column.
  EXPECT_EQ(row(run(s, "SELECT b FROM kv_d.t ORDER BY c")), (std::vector<std::string>{"2"}));
}

// Leading blanks and control characters leave a name; an alias keeps at most
// 256 bytes, a character whole, and an alias changed is warned of, 1466 with
// its unchanged text or 1474 when nothing is left; a generated name changes
// without a warning (observed on MySQL 8.4.13).
TEST(ExecSession, AnAliasIsTrimmedAndCutWithAWarning) {
  auto s = make();
  const auto names = [](const Outcome& o) {
    std::vector<std::string> out;
    for (const auto& c : o.result->columns) out.push_back(c.name);
    return out;
  };
  auto o = run(s, "SELECT 1 AS '  x', '  y', 2 AS '\tt'");
  EXPECT_EQ(names(o), (std::vector<std::string>{"x", "y", "t"}));
  ASSERT_EQ(s.warnings().size(), 2u);
  EXPECT_EQ(s.warnings()[0].code, 1466);
  EXPECT_EQ(s.warnings()[0].message, "Leading spaces are removed from name '  x'");
  EXPECT_EQ(s.warnings()[1].message, "Leading spaces are removed from name '\tt'");

  o = run(s, "SELECT 1 AS ' '");
  EXPECT_EQ(names(o), (std::vector<std::string>{""}));
  ASSERT_EQ(s.warnings().size(), 1u);
  EXPECT_EQ(s.warnings()[0].code, 1474);
  EXPECT_EQ(s.warnings()[0].message, "Name ' ' has become ''");

  const std::string b(300, 'b');
  std::string ae = "a";
  for (int i = 0; i < 200; ++i) ae += "é";
  o = run(s, "SELECT 1 AS " + b + ", 2 AS `" + ae + "`");
  EXPECT_EQ(names(o)[0], std::string(256, 'b'));
  EXPECT_EQ(names(o)[1], ae.substr(0, 255));
  ASSERT_EQ(s.warnings().size(), 2u);
  EXPECT_EQ(s.warnings()[0].message, "Leading spaces are removed from name '" + b + "'");

  // From a table, the same.
  run(s, "CREATE DATABASE kv_alias");
  run(s, "CREATE TABLE kv_alias.t (a INT)");
  o = run(s, "SELECT a AS '  z' FROM kv_alias.t");
  EXPECT_EQ(names(o), (std::vector<std::string>{"z"}));
  EXPECT_EQ(s.warnings().size(), 1u);
  run(s, "DROP DATABASE kv_alias");
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

// Observed on MySQL 8.4.13, 2026-11-19: between two IMPLICIT strings,
// utf8mb4 outranks utf8mb3, its superset, even utf8mb3_bin (v = mb compares
// without case); a tie of two utf8mb4 or two utf8mb3 collations is as
// before.
TEST(ExecSessionTables, Utf8mb4OutranksUtf8mb3) {
  TableBed b;
  run(b.s, "CREATE TABLE sup (v VARCHAR(10), m VARCHAR(4) CHARACTER SET utf8mb3, mb VARCHAR(4) CHARACTER SET utf8mb3 COLLATE utf8mb3_bin)");
  run(b.s, "INSERT INTO sup VALUES ('M', 'm', 'm')");
  auto o = run(b.s, "SELECT CONCAT(m, v), v = m, m = v, v < m, CONCAT(m, 'a'), CONCAT(mb, v), v = mb, IFNULL(m, v), v LIKE m FROM sup");
  EXPECT_EQ(row(o), (std::vector<std::string>{"mM", "1", "1", "0", "ma", "mM", "1", "m", "1"}));
  const Shape compared{f::longlong, bin, 1, 0, binflag | num};
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::var_string, ai, 56, 31, 0},
                                           compared,
                                           compared,
                                           compared,
                                           {f::var_string, ai, 20, 31, 0},
                                           {f::var_string, ai, 56, 31, 0},
                                           compared,
                                           {f::var_string, ai, 40, 31, 0},
                                           compared}));
  o = run(b.s, "SELECT COALESCE(v, m, mb) FROM sup");
  EXPECT_EQ(row(o), (std::vector<std::string>{"M"}));
  EXPECT_EQ(shapes(o), (std::vector<Shape>{{f::var_string, ai, 40, 31, 0}}));
}

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

// UPDATE and DELETE, observed on MySQL 8.4.13 (2026-11-16): UPDATE counts
// the rows it changed, says how many matched, and sets columns in the order
// written, each seeing those before; a raised AUTO_INCREMENT value carries
// the counter past it; DELETE removes row by row.
TEST(ExecSessionRows, UpdateAndDelete) {
  TableBed b;
  run(b.s, "CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, a INT, b VARCHAR(3) DEFAULT 'x', d INT NOT NULL, u INT UNIQUE)");
  run(b.s, "INSERT INTO t (a, d, u) VALUES (1, 10, 1), (2, 20, 2), (3, 30, 3), (4, 40, NULL)");
  auto o = run(b.s, "UPDATE t SET a = a + 10 WHERE id <= 2");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(o.info, "Rows matched: 2  Changed: 2  Warnings: 0");
  EXPECT_EQ(row(run(b.s, "SELECT ROW_COUNT()")), (std::vector<std::string>{"2"}));
  o = run(b.s, "UPDATE t SET a = a WHERE id = 1");
  EXPECT_EQ(o.affected_rows, 0u);
  EXPECT_EQ(o.info, "Rows matched: 1  Changed: 0  Warnings: 0");
  b.expect_refused("UPDATE t SET b = 'abcd' WHERE id = 1", 1406, "22001", "Data too long for column 'b' at row 1");
  b.expect_refused("UPDATE t SET d = NULL WHERE id = 1", 1048, "23000", "Column 'd' cannot be null");
  b.expect_refused("UPDATE t SET u = 2 WHERE id = 1", 1062, "23000", "Duplicate entry '2' for key 't.u'");
  b.expect_refused("UPDATE t SET id = 2 WHERE id = 1", 1062, "23000", "Duplicate entry '2' for key 't.PRIMARY'");
  b.expect_refused("UPDATE t SET zz = 1", 1054, "42S22", "Unknown column 'zz' in 'field list'");
  b.expect_refused("UPDATE t SET a = 'x' WHERE id = 1", 1366, "HY000", "Incorrect integer value: 'x' for column 'a' at row 1");
  run(b.s, "UPDATE t SET b = DEFAULT WHERE id = 3");
  o = run(b.s, "UPDATE t SET a = 5, b = 'yy' WHERE a > 100");
  EXPECT_EQ(o.info, "Rows matched: 0  Changed: 0  Warnings: 0");
  run(b.s, "UPDATE t SET a = a + 1 ORDER BY id DESC LIMIT 2");
  run(b.s, "UPDATE t SET id = id + 10 WHERE id = 4");
  run(b.s, "UPDATE t SET b = 'ab ' WHERE id = 2");
  EXPECT_TRUE(b.s.warnings().empty());
  run(b.s, "UPDATE t SET a = NULL, b = NULL WHERE id = 3");
  b.expect_refused("UPDATE nothere SET a = 1", 1146, "42S02", "Table 'pt.nothere' doesn't exist");
  run(b.s, "UPDATE t AS x SET x.a = 7 WHERE x.id = 2");
  b.expect_refused("UPDATE t SET a = 1 WHERE zz = 1", 1054, "42S22", "Unknown column 'zz' in 'where clause'");
  b.expect_refused("UPDATE t SET d = DEFAULT WHERE id = 2", 1364, "HY000", "Field 'd' doesn't have a default value");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM t")),
            (Rows{{"1", "11", "x", "10", "1"}, {"2", "7", "ab ", "20", "2"}, {"3", "NULL", "NULL", "30", "3"}, {"14", "5", "x", "40", "NULL"}}));
  o = run(b.s, "DELETE FROM t WHERE id = 2");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.info, "");
  EXPECT_EQ(run(b.s, "DELETE FROM t WHERE a > 1000").affected_rows, 0u);
  run(b.s, "DELETE FROM t ORDER BY id DESC LIMIT 1");
  b.expect_refused("DELETE FROM nothere", 1146, "42S02", "Table 'pt.nothere' doesn't exist");
  b.expect_refused("DELETE FROM t WHERE zz = 1", 1054, "42S22", "Unknown column 'zz' in 'where clause'");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM t")), (Rows{{"1", "11", "x", "10", "1"}, {"3", "NULL", "NULL", "30", "3"}}));
  // The key and the unique value of a row deleted are free again.
  run(b.s, "INSERT INTO t (id, a, d, u) VALUES (2, 9, 9, 2)");
  EXPECT_EQ(run(b.s, "DELETE FROM t").affected_rows, 3u);
  // The counter went past 14, which an UPDATE set.
  EXPECT_EQ(run(b.s, "INSERT INTO t (a, d) VALUES (1, 1)").last_insert_id, 15u);

  run(b.s, "CREATE TABLE n (a INT, b INT)");
  run(b.s, "INSERT INTO n VALUES (1, 1), (2, 2), (1, 3)");
  run(b.s, "UPDATE n SET b = b * 10 WHERE a = 1");
  run(b.s, "DELETE FROM n WHERE b = 2");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM n")), (Rows{{"1", "10"}, {"1", "30"}}));
  run(b.s, "UPDATE n SET a = 1, a = 2");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM n")), (Rows{{"2", "10"}, {"2", "30"}}));
  run(b.s, "UPDATE n SET b = a, a = b");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM n")), (Rows{{"2", "2"}, {"2", "2"}}));
}

// UPDATE and DELETE in transactions: they build on the latest commit, not
// the snapshot (InnoDB's current read), so no change is lost; a row another
// transaction holds must be waited for; ROLLBACK brings rows back.
TEST(ExecSessionRows, UpdatesInTransactions) {
  TableBed b;
  auto other = b.other();
  run(b.s, "CREATE TABLE c (id INT PRIMARY KEY, n INT)");
  run(b.s, "INSERT INTO c VALUES (1, 0)");
  run(b.s, "BEGIN");
  EXPECT_EQ(rows_of(run(b.s, "SELECT n FROM c")), (Rows{{"0"}}));
  run(other, "UPDATE c SET n = n + 1");
  // The snapshot still says 0; the UPDATE adds to the 1 committed.
  EXPECT_EQ(rows_of(run(b.s, "SELECT n FROM c")), (Rows{{"0"}}));
  run(b.s, "UPDATE c SET n = n + 1");
  EXPECT_EQ(rows_of(run(b.s, "SELECT n FROM c")), (Rows{{"2"}}));
  // Its row is locked until it ends.
  EXPECT_THROW(run(other, "UPDATE c SET n = 100"), LockWaitNeeded);
  EXPECT_THROW(run(other, "DELETE FROM c"), LockWaitNeeded);
  run(b.s, "COMMIT");
  EXPECT_EQ(rows_of(run(other, "SELECT n FROM c")), (Rows{{"2"}}));
  run(b.s, "BEGIN");
  run(b.s, "DELETE FROM c");
  EXPECT_TRUE(run(b.s, "SELECT n FROM c").result->rows.empty());
  EXPECT_EQ(rows_of(run(other, "SELECT n FROM c")), (Rows{{"2"}}));
  run(b.s, "ROLLBACK");
  EXPECT_EQ(rows_of(run(b.s, "SELECT n FROM c")), (Rows{{"2"}}));
}

// Locking reads, as two sessions saw them on MySQL 8.4.13: a plain read never
// waits; FOR UPDATE and FOR SHARE NOWAIT on a held row are 3572; SKIP LOCKED
// leaves held rows out; a held row makes the reader wait; shared locks share
// and keep an UPDATE out; a locking read sees the latest commit where the
// snapshot does not; OF names the table (3568), a table takes one clause
// (3569); a locking read keeps others' inserts out of the table's keys,
// but under READ COMMITTED.
TEST(ExecSessionRows, LockingReads) {
  TableBed b;
  auto other = b.other();
  run(b.s, "CREATE TABLE lk (id INT PRIMARY KEY, n INT)");
  run(b.s, "INSERT INTO lk VALUES (1, 10), (2, 20), (3, 30)");
  const std::string nowait = "Statement aborted because lock(s) could not be acquired immediately and NOWAIT is set.";

  run(b.s, "BEGIN");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM lk WHERE id = 1 FOR UPDATE")), (Rows{{"1", "10"}}));
  EXPECT_EQ(rows_of(run(other, "SELECT * FROM lk WHERE id = 1")), (Rows{{"1", "10"}}));
  auto r = refusal(other, "SELECT * FROM lk WHERE id = 1 FOR UPDATE NOWAIT");
  EXPECT_EQ(r.code, 3572);
  EXPECT_EQ(r.sqlstate, "HY000");
  EXPECT_EQ(r.message, nowait);
  EXPECT_EQ(refusal(other, "SELECT * FROM lk WHERE id = 1 FOR SHARE NOWAIT").code, 3572);
  EXPECT_EQ(rows_of(run(other, "SELECT * FROM lk FOR UPDATE SKIP LOCKED")), (Rows{{"2", "20"}, {"3", "30"}}));
  EXPECT_THROW(run(other, "SELECT * FROM lk WHERE id = 1 FOR UPDATE"), LockWaitNeeded);
  run(other, "UPDATE lk SET n = 21 WHERE id = 2");
  run(b.s, "COMMIT");

  run(b.s, "BEGIN");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM lk WHERE id = 2 FOR SHARE")), (Rows{{"2", "21"}}));
  EXPECT_EQ(rows_of(run(other, "SELECT * FROM lk WHERE id = 2 FOR SHARE")), (Rows{{"2", "21"}}));
  EXPECT_EQ(rows_of(run(other, "SELECT * FROM lk WHERE id = 2 LOCK IN SHARE MODE")), (Rows{{"2", "21"}}));
  EXPECT_THROW(run(other, "UPDATE lk SET n = 22 WHERE id = 2"), LockWaitNeeded);
  run(other, "UPDATE lk SET n = 33 WHERE id = 3");
  run(b.s, "COMMIT");

  run(b.s, "BEGIN");
  EXPECT_EQ(rows_of(run(b.s, "SELECT n FROM lk WHERE id = 3")), (Rows{{"33"}}));
  run(other, "UPDATE lk SET n = 34 WHERE id = 3");
  EXPECT_EQ(rows_of(run(b.s, "SELECT n FROM lk WHERE id = 3")), (Rows{{"33"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT n FROM lk WHERE id = 3 FOR SHARE")), (Rows{{"34"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT COUNT(*) FROM lk FOR SHARE")), (Rows{{"3"}}));
  run(b.s, "COMMIT");

  EXPECT_EQ(rows_of(run(b.s, "SELECT id FROM lk FOR UPDATE OF lk")), (Rows{{"1"}, {"2"}, {"3"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT id FROM lk AS x WHERE id = 1 FOR UPDATE OF x")), (Rows{{"1"}}));
  r = refusal(b.s, "SELECT * FROM lk FOR UPDATE OF u");
  EXPECT_EQ(r.code, 3568);
  EXPECT_EQ(r.message, "Unresolved table name `u` in locking clause.");
  r = refusal(b.s, "SELECT * FROM lk FOR SHARE FOR UPDATE");
  EXPECT_EQ(r.code, 3569);
  EXPECT_EQ(r.message, "Table lk appears in multiple locking clauses.");
  EXPECT_THROW(run(b.s, "SELECT * FROM lk LOCK IN SHARE MODE NOWAIT"), sql::SyntaxError);
  EXPECT_THROW(run(b.s, "SELECT * FROM lk FOR UPDATE NOWAIT SKIP LOCKED"), sql::SyntaxError);
  EXPECT_EQ(rows_of(run(b.s, "SELECT 1 FOR UPDATE")), (Rows{{"1"}}));

  // The keys read are kept from others' inserts until the reader ends.
  run(b.s, "BEGIN");
  run(b.s, "SELECT * FROM lk WHERE n > 100 FOR UPDATE");
  EXPECT_THROW(run(other, "INSERT INTO lk VALUES (9, 90)"), LockWaitNeeded);
  run(b.s, "COMMIT");
  run(other, "INSERT INTO lk VALUES (9, 90)");
  // READ COMMITTED takes no such locks.
  run(b.s, "SET SESSION transaction_isolation = 'READ-COMMITTED'");
  run(b.s, "BEGIN");
  run(b.s, "SELECT * FROM lk WHERE n > 100 FOR UPDATE");
  run(other, "INSERT INTO lk VALUES (8, 80)");
  run(b.s, "COMMIT");

  // A WHERE that bounds the integer primary key locks only that stretch of
  // keys, as InnoDB's next-key locks over the index do.
  run(b.s, "SET SESSION transaction_isolation = 'REPEATABLE-READ'");
  run(b.s, "BEGIN");
  EXPECT_EQ(rows_of(run(b.s, "SELECT id FROM lk WHERE id = 2 FOR UPDATE")), (Rows{{"2"}}));
  run(other, "INSERT INTO lk VALUES (5, 50)");
  EXPECT_EQ(rows_of(run(b.s, "SELECT id FROM lk WHERE id BETWEEN 3 AND 7 AND n >= 0 FOR SHARE")), (Rows{{"3"}, {"5"}}));
  EXPECT_THROW(run(other, "INSERT INTO lk VALUES (6, 60)"), LockWaitNeeded);
  run(other, "INSERT INTO lk VALUES (10, 100)");
  EXPECT_EQ(rows_of(run(b.s, "SELECT id FROM lk WHERE id IN (11, 13) FOR SHARE")), (Rows{}));
  EXPECT_THROW(run(other, "INSERT INTO lk VALUES (12, 120)"), LockWaitNeeded);
  run(other, "INSERT INTO lk VALUES (14, 140)");
  run(b.s, "COMMIT");
  run(other, "INSERT INTO lk VALUES (6, 60)");
}

// INSERT IGNORE's OK: only the rows inserted are affected; several rows give
// "Records: n  Duplicates: d  Warnings: w"; a skipped row's generated value
// is the next row's, and the statement's id is the first row inserted's
// (observed on MySQL 8.4.13 through PyMySQL).
TEST(ExecSessionRows, InsertIgnoreCountsWhatItInserted) {
  TableBed b;
  run(b.s, "CREATE TABLE g (id INT AUTO_INCREMENT PRIMARY KEY, u VARCHAR(3) UNIQUE, n INT NOT NULL)");
  run(b.s, "INSERT INTO g VALUES (1, 'a', 1)");
  auto o = run(b.s, "INSERT IGNORE INTO g VALUES (1, 'b', 2), (2, 'a', 3), (3, 'c', 4)");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.info, "Records: 3  Duplicates: 2  Warnings: 2");
  o = run(b.s, "INSERT IGNORE INTO g VALUES (1, 'b', 2)");
  EXPECT_EQ(o.affected_rows, 0u);
  EXPECT_EQ(o.info, "");
  EXPECT_EQ(b.s.warnings().size(), 1u);
  o = run(b.s, "INSERT IGNORE INTO g (u, n) VALUES ('dddd', NULL)");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.last_insert_id, 4u);
  EXPECT_EQ(b.s.warnings().size(), 2u);
  o = run(b.s, "INSERT IGNORE INTO g (u, n) VALUES ('c', 1), ('e', 1)");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.info, "Records: 2  Duplicates: 1  Warnings: 1");
  EXPECT_EQ(o.last_insert_id, 5u);
  o = run(b.s, "INSERT IGNORE INTO g (u, n) VALUES ('c', 1)");
  EXPECT_EQ(o.affected_rows, 0u);
  EXPECT_EQ(o.last_insert_id, 0u);
  o = run(b.s, "INSERT IGNORE INTO g (u, n) VALUES ('h', 1)");
  EXPECT_EQ(o.last_insert_id, 8u);
  EXPECT_EQ(rows_of(run(b.s, "SELECT id, u, n FROM g ORDER BY id")),
            (Rows{{"1", "a", "1"}, {"3", "c", "4"}, {"4", "ddd", "0"}, {"5", "e", "1"}, {"8", "h", "1"}}));
}

// REPLACE deletes each row its new row would duplicate, by any unique key,
// then inserts it: each deleted row and each inserted counts; several rows
// give "Records: n  Duplicates: <deleted>  Warnings: w"; a generated id is
// always new. The OK's id is the first generated, else the AUTO_INCREMENT
// value given (observed on MySQL 8.4.13 through PyMySQL).
TEST(ExecSessionRows, ReplaceDeletesWhatItsRowWouldDuplicate) {
  TableBed b;
  run(b.s, "CREATE TABLE r (id INT AUTO_INCREMENT PRIMARY KEY, u VARCHAR(3) UNIQUE, n INT NOT NULL DEFAULT 7)");
  auto o = run(b.s, "INSERT INTO r VALUES (1, 'a', 1), (2, 'b', 2), (3, 'c', 3)");
  EXPECT_EQ(o.last_insert_id, 3u);
  o = run(b.s, "REPLACE INTO r VALUES (4, 'd', 4)");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.last_insert_id, 4u);
  o = run(b.s, "REPLACE INTO r VALUES (1, 'a', 10)");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(o.last_insert_id, 1u);
  o = run(b.s, "REPLACE INTO r VALUES (2, 'c', 20)");
  EXPECT_EQ(o.affected_rows, 3u);
  EXPECT_EQ(rows_of(run(b.s, "SELECT id, u, n FROM r ORDER BY id")), (Rows{{"1", "a", "10"}, {"2", "c", "20"}, {"4", "d", "4"}}));
  o = run(b.s, "REPLACE INTO r (u, n) VALUES ('d', 40), ('e', 50), ('e', 51)");
  EXPECT_EQ(o.affected_rows, 5u);
  EXPECT_EQ(o.info, "Records: 3  Duplicates: 2  Warnings: 0");
  EXPECT_EQ(o.last_insert_id, 5u);
  o = run(b.s, "REPLACE r SET id = 9, u = 'z'");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.last_insert_id, 9u);
  o = run(b.s, "REPLACE INTO r (u) VALUES ('a')");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(o.last_insert_id, 10u);
  EXPECT_EQ(rows_of(run(b.s, "SELECT id, u, n FROM r ORDER BY id")),
            (Rows{{"2", "c", "20"}, {"5", "d", "40"}, {"7", "e", "51"}, {"9", "z", "7"}, {"10", "a", "7"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT LAST_INSERT_ID()")), (Rows{{"10"}}));
  b.expect_refused("REPLACE INTO r (u, n) VALUES ('q', NULL)", 1048, "23000", "Column 'n' cannot be null");
}

// INSERT ... ON DUPLICATE KEY UPDATE: a row inserted is 1 affected, a row
// updated 2, one the assignments leave as it was 0; several rows give
// "Records: n  Duplicates: d  Warnings: w"; the OK's id is a generated
// value, else the updated row's AUTO_INCREMENT value; LAST_INSERT_ID()
// moves only for a generated one (observed on MySQL 8.4.13 through PyMySQL).
TEST(ExecSessionRows, OnDuplicateKeyUpdateUpdatesTheRowAKeyHolds) {
  TableBed b;
  run(b.s, "CREATE TABLE p (id INT AUTO_INCREMENT PRIMARY KEY, u VARCHAR(3) UNIQUE, n INT NOT NULL DEFAULT 0)");
  run(b.s, "INSERT INTO p VALUES (1, 'a', 1), (2, 'b', 2)");
  auto o = run(b.s, "INSERT INTO p (u, n) VALUES ('c', 3) ON DUPLICATE KEY UPDATE n = n + 100");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.last_insert_id, 3u);
  o = run(b.s, "INSERT INTO p (u, n) VALUES ('a', 5) ON DUPLICATE KEY UPDATE n = n + 100");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(o.last_insert_id, 1u);
  o = run(b.s, "INSERT INTO p (u, n) VALUES ('a', 5) ON DUPLICATE KEY UPDATE n = n");
  EXPECT_EQ(o.affected_rows, 0u);
  EXPECT_EQ(o.last_insert_id, 0u);
  o = run(b.s, "INSERT INTO p (u, n) VALUES ('b', 7) ON DUPLICATE KEY UPDATE n = VALUES(n)");
  EXPECT_EQ(o.affected_rows, 2u);
  ASSERT_EQ(b.s.warnings().size(), 1u);
  EXPECT_EQ(b.s.warnings()[0].code, 1287);
  run(b.s, "INSERT INTO p (u, n) VALUES ('b', 8) AS new ON DUPLICATE KEY UPDATE n = new.n + p.n");
  run(b.s, "INSERT INTO p (u, n) VALUES ('b', 9) AS new (x, y) ON DUPLICATE KEY UPDATE n = y + new.y");
  EXPECT_EQ(rows_of(run(b.s, "SELECT id, u, n FROM p ORDER BY id")), (Rows{{"1", "a", "101"}, {"2", "b", "18"}, {"3", "c", "3"}}));
  o = run(b.s, "INSERT INTO p (u, n) VALUES ('a', 1), ('d', 4), ('a', 2) ON DUPLICATE KEY UPDATE n = n + VALUES(n)");
  EXPECT_EQ(o.affected_rows, 5u);
  EXPECT_EQ(o.info, "Records: 3  Duplicates: 2  Warnings: 1");
  EXPECT_EQ(o.last_insert_id, 9u);
  b.expect_refused("INSERT INTO p (u, n) VALUES ('a', 1) ON DUPLICATE KEY UPDATE u = 'b'", 1062, "23000", "Duplicate entry 'b' for key 'p.u'");
  o = run(b.s, "INSERT INTO p (u, n) VALUES ('a', 1) ON DUPLICATE KEY UPDATE id = id + 10, n = id");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(o.last_insert_id, 11u);
  b.expect_refused("INSERT INTO p (u, n) VALUES ('z', 1) ON DUPLICATE KEY UPDATE nope = 1", 1054, "42S22", "Unknown column 'nope' in 'field list'");
  b.expect_refused("INSERT INTO p (u, n) VALUES ('a', 1) ON DUPLICATE KEY UPDATE n = NULL", 1048, "23000", "Column 'n' cannot be null");
  o = run(b.s, "INSERT IGNORE INTO p (u, n) VALUES ('a', 1) ON DUPLICATE KEY UPDATE n = NULL");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(b.s.warnings().size(), 1u);
  EXPECT_EQ(rows_of(run(b.s, "SELECT id, u, n FROM p ORDER BY id")), (Rows{{"2", "b", "18"}, {"3", "c", "3"}, {"9", "d", "4"}, {"11", "a", "0"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT LAST_INSERT_ID()")), (Rows{{"9"}}));
}

// INSERT ... SELECT and REPLACE ... SELECT as PyMySQL saw them on MySQL
// 8.4.13: the info line always; AUTO_INCREMENT values reserved 1, 2, 4 at a
// time, as rows not counted ahead; the OK's id only a generated one; a
// table's column copied too long is 1265, an expression 1406; a table may
// feed itself.
TEST(ExecSessionRows, InsertSelect) {
  TableBed b;
  run(b.s, "CREATE TABLE s (a INT, b VARCHAR(5))");
  run(b.s, "CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, a INT, b VARCHAR(3) UNIQUE)");
  run(b.s, "INSERT INTO s VALUES (1, 'x'), (2, 'y'), (3, 'zzzz'), (4, 'x')");
  auto o = run(b.s, "INSERT INTO t (a, b) SELECT a, b FROM s WHERE a = 1");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.info, "Records: 1  Duplicates: 0  Warnings: 0");
  EXPECT_EQ(o.last_insert_id, 1u);
  o = run(b.s, "INSERT INTO t (a, b) SELECT a, b FROM s WHERE a = 2 OR a = 9");
  EXPECT_EQ(o.last_insert_id, 2u);
  b.expect_refused("INSERT INTO t (a, b) SELECT a, b FROM s WHERE a = 3", 1265, "01000", "Data truncated for column 'b' at row 1");
  o = run(b.s, "INSERT IGNORE INTO t (a, b) SELECT a, b FROM s");
  EXPECT_EQ(o.affected_rows, 1u);
  EXPECT_EQ(o.info, "Records: 4  Duplicates: 3  Warnings: 4");
  EXPECT_EQ(o.last_insert_id, 3u);
  ASSERT_EQ(b.s.warnings().size(), 4u);
  EXPECT_EQ(b.s.warnings()[2].message, "Data truncated for column 'b' at row 3");
  o = run(b.s, "INSERT INTO t (a, b) SELECT a, b FROM s WHERE a > 100");
  EXPECT_EQ(o.affected_rows, 0u);
  EXPECT_EQ(o.info, "Records: 0  Duplicates: 0  Warnings: 0");
  b.expect_refused("INSERT INTO t (a) SELECT a, b FROM s", 1136, "21S01", "Column count doesn't match value count at row 1");
  o = run(b.s, "INSERT INTO t SELECT NULL, a + 10, CONCAT(b, a) FROM s WHERE a <= 2 ORDER BY a DESC LIMIT 1");
  EXPECT_EQ(o.last_insert_id, 6u);
  o = run(b.s, "INSERT INTO t (a, b) SELECT a, b FROM s WHERE a = 1 ON DUPLICATE KEY UPDATE a = t.a + 100");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(o.info, "Records: 1  Duplicates: 1  Warnings: 0");
  EXPECT_EQ(o.last_insert_id, 0u);
  o = run(b.s, "INSERT INTO t (a, b) SELECT a, b FROM s WHERE a = 2 ON DUPLICATE KEY UPDATE a = VALUES(a) + 200");
  EXPECT_EQ(o.info, "Records: 1  Duplicates: 1  Warnings: 1");
  o = run(b.s, "REPLACE INTO t (a, b) SELECT a, b FROM s WHERE a = 2");
  EXPECT_EQ(o.affected_rows, 2u);
  EXPECT_EQ(o.info, "Records: 1  Duplicates: 1  Warnings: 0");
  EXPECT_EQ(o.last_insert_id, 9u);
  b.expect_refused("INSERT INTO t (a, b) SELECT COUNT(*), MAX(b) FROM s", 1406, "22001", "Data too long for column 'b' at row 1");
  b.expect_refused("INSERT INTO t (a, b) SELECT a, b FROM t WHERE id = 1", 1062, "23000", "Duplicate entry 'x' for key 't.b'");
  o = run(b.s, "INSERT INTO t (a, b) SELECT a + 1000, CONCAT('q', id) FROM t");
  EXPECT_EQ(o.affected_rows, 4u);
  EXPECT_EQ(o.info, "Records: 4  Duplicates: 0  Warnings: 0");
  EXPECT_EQ(o.last_insert_id, 11u);
  b.expect_refused("INSERT INTO t (a, b) SELECT 'nine', 'n'", 1366, "HY000", "Incorrect integer value: 'nine' for column 'a' at row 1");
  o = run(b.s, "INSERT INTO t (a, b) (SELECT 5, 'p')");
  EXPECT_EQ(o.last_insert_id, 18u);
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM t ORDER BY id")), (Rows{{"1", "101", "x"},
                                                                    {"3", "3", "zzz"},
                                                                    {"6", "12", "y2"},
                                                                    {"9", "2", "y"},
                                                                    {"11", "1101", "q1"},
                                                                    {"12", "1003", "q3"},
                                                                    {"13", "1012", "q6"},
                                                                    {"14", "1002", "q9"},
                                                                    {"18", "5", "p"}}));
  EXPECT_EQ(rows_of(run(b.s, "SELECT LAST_INSERT_ID()")), (Rows{{"18"}}));
}

// INSERT ... SELECT reads its source with shared locks and the source's key
// range, but under READ COMMITTED (the manual, "Locks Set by Different SQL
// Statements in InnoDB").
TEST(ExecSessionRows, InsertSelectLocksItsSource) {
  TableBed b;
  auto other = b.other();
  run(b.s, "CREATE TABLE src (id INT PRIMARY KEY, n INT)");
  run(b.s, "CREATE TABLE dst (id INT PRIMARY KEY, n INT)");
  run(b.s, "INSERT INTO src VALUES (1, 10), (2, 20), (5, 50)");
  run(b.s, "BEGIN");
  run(b.s, "INSERT INTO dst SELECT * FROM src WHERE id <= 2");
  EXPECT_EQ(rows_of(run(other, "SELECT n FROM src WHERE id = 1")), (Rows{{"10"}}));
  EXPECT_EQ(rows_of(run(other, "SELECT n FROM src WHERE id = 1 FOR SHARE")), (Rows{{"10"}}));
  EXPECT_THROW(run(other, "UPDATE src SET n = 11 WHERE id = 1"), LockWaitNeeded);
  EXPECT_THROW(run(other, "INSERT INTO src VALUES (0, 0)"), LockWaitNeeded);
  run(other, "UPDATE src SET n = 51 WHERE id = 5");
  run(other, "INSERT INTO src VALUES (7, 70)");
  run(b.s, "COMMIT");
  run(other, "UPDATE src SET n = 11 WHERE id = 1");

  run(b.s, "SET SESSION transaction_isolation = 'READ-COMMITTED'");
  run(b.s, "BEGIN");
  run(b.s, "INSERT INTO dst SELECT id + 10, n FROM src WHERE id <= 2");
  run(other, "UPDATE src SET n = 12 WHERE id = 1");
  run(b.s, "COMMIT");
  EXPECT_EQ(rows_of(run(b.s, "SELECT * FROM dst ORDER BY id")), (Rows{{"1", "10"}, {"2", "20"}, {"11", "11"}, {"12", "20"}}));
}

TEST(ExecSessionRows, InsertsKvasirDoesNotYetCarryOut) {
  TableBed b;
  run(b.s, "CREATE TABLE n (a INT, b INT)");
  b.expect_refused("INSERT INTO n TABLE n", 1235, "42000", "This version of Kvasir doesn't yet support 'INSERT ... TABLE'");
  b.expect_refused("REPLACE INTO n TABLE n", 1235, "42000", "This version of Kvasir doesn't yet support 'REPLACE ... TABLE'");
  b.expect_refused("INSERT INTO n (a, b) VALUES (5, a)", 1235, "42000", "This version of Kvasir doesn't yet support 'columns in VALUES'");
}

using Shapes = std::vector<Shape>;
constexpr uint16_t group_flag = 32768;  // GROUP_FLAG, NUM's bit

// The tables the aggregate probes ran over (MySQL 8.4.13, 2026-11-17).
struct GroupBed : TableBed {
  GroupBed() {
    run(s, "CREATE TABLE t (id INT PRIMARY KEY, g VARCHAR(10), i INT, u INT UNSIGNED, b BIGINT, d DECIMAL(6,2), f DOUBLE, s VARCHAR(20), ti TINYINT)");
    run(s,
        "INSERT INTO t VALUES (1,'a',10,1,100,1.50,1.5,'x',1),(2,'a',20,2,200,2.25,2.5,'y',2),(3,'b',NULL,3,NULL,NULL,NULL,NULL,NULL),"
        "(4,'b',5,4,5,0.10,0.1,'z',3),(5,NULL,7,5,7,7.00,7,'w',4)");
    run(s,
        "CREATE TABLE v (id INT PRIMARY KEY, k INT NOT NULL, n INT, d DECIMAL(6,2), f DOUBLE, c CHAR(3), s VARCHAR(5) NOT NULL DEFAULT '', z INT(4) "
        "ZEROFILL, bs VARBINARY(4), tx TEXT)");
    run(s, "INSERT INTO v VALUES (1,1,1,1.00,1,'a','x',1,'p','t'),(2,1,2,2.00,2,'a','y',2,'q','u'),(3,2,3,3.00,3,'b','z',3,'r','v')");
    run(s, "CREATE TABLE w (id INT PRIMARY KEY, a CHAR(2) NOT NULL, b VARCHAR(3), c VARCHAR(3) NOT NULL, e TEXT, i INT, d DECIMAL(4,1) NOT NULL, x DOUBLE)");
    run(s, "INSERT INTO w VALUES (1,'p','q','r','s',1,1.0,1),(2,'p','q','r','s',2,2.0,2)");
  }
  Rows rows(const std::string& sql) { return rows_of(run(s, sql)); }
  Shapes shapes_of(const std::string& sql) { return shapes(run(s, sql)); }
};

// The manual's "Aggregate Function Descriptions": COUNT, SUM, AVG, MIN and
// MAX, with DISTINCT; NULL skipped; over no rows COUNT is 0 and the others
// NULL; a query with an aggregate and no GROUP BY is one group. Types as
// observed: COUNT a BIGINT; SUM of an exact value DECIMAL with 22 more
// digits, AVG with div_precision_increment more digits and decimals; of a
// DOUBLE or a string, DOUBLE (a string read as a number, warning 1292); MIN
// and MAX the argument's own type.
TEST(ExecSessionGroups, Aggregates) {
  GroupBed b;
  auto o = run(b.s, "SELECT COUNT(*), COUNT(i), COUNT(DISTINCT g), SUM(i), SUM(u), SUM(b), SUM(d), SUM(f), SUM(ti) FROM t");
  EXPECT_EQ(rows_of(o), (Rows{{"5", "4", "2", "42", "15", "312", "10.85", "11.1", "10"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::longlong, bin, 21, 0, nn | binflag | num},
                               {f::longlong, bin, 21, 0, nn | binflag | num},
                               {f::longlong, bin, 21, 0, nn | binflag | num},
                               {f::newdecimal, bin, 33, 0, binflag | num},
                               {f::newdecimal, bin, 33, 0, binflag | num},
                               {f::newdecimal, bin, 42, 0, binflag | num},
                               {f::newdecimal, bin, 30, 2, binflag | num},
                               {f::double_, bin, 23, 31, binflag | num},
                               {f::newdecimal, bin, 26, 0, binflag | num}}));
  o = run(b.s, "SELECT AVG(i), AVG(u), AVG(b), AVG(d), AVG(f), AVG(ti) FROM t");
  EXPECT_EQ(rows_of(o), (Rows{{"10.5000", "3.0000", "78.0000", "2.712500", "2.775", "2.5000"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::newdecimal, bin, 16, 4, binflag | num},
                               {f::newdecimal, bin, 16, 4, binflag | num},
                               {f::newdecimal, bin, 25, 4, binflag | num},
                               {f::newdecimal, bin, 12, 6, binflag | num},
                               {f::double_, bin, 23, 31, binflag | num},
                               {f::newdecimal, bin, 9, 4, binflag | num}}));
  o = run(b.s, "SELECT MIN(i), MAX(i), MIN(u), MAX(d), MIN(f), MAX(s), MIN(g), MAX(ti), MIN(b) FROM t");
  EXPECT_EQ(rows_of(o), (Rows{{"5", "20", "1", "7.00", "0.1", "z", "a", "4", "5"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::long_, bin, 11, 0, binflag | num},
                               {f::long_, bin, 11, 0, binflag | num},
                               {f::long_, bin, 10, 0, uns | binflag | num},
                               {f::newdecimal, bin, 8, 2, binflag | num},
                               {f::double_, bin, 22, 31, binflag | num},
                               {f::var_string, ai, 80, 31, 0},
                               {f::var_string, ai, 40, 31, 0},
                               {f::tiny, bin, 4, 0, binflag | num},
                               {f::longlong, bin, 20, 0, binflag | num}}));
  // MIN and MAX of a column: no ZEROFILL, BLOB or key flags, a TEXT's length
  // counted in characters once more.
  o = run(b.s, "SELECT MIN(z), MAX(c), MIN(bs), MIN(tx), MAX(f), MIN(n) FROM v");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "b", "p", "t", "3", "1"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::long_, bin, 10, 0, uns | binflag | num},
                               {f::string, ai, 12, 31, 0},
                               {f::var_string, bin, 4, 31, binflag},
                               {f::blob, ai, 1048560, 31, 0},
                               {f::double_, bin, 22, 31, binflag | num},
                               {f::long_, bin, 11, 0, binflag | num}}));
  EXPECT_EQ(b.rows("SELECT COUNT(*), SUM(i), AVG(i), MIN(i), MAX(s) FROM t WHERE id > 100"), (Rows{{"0", "NULL", "NULL", "NULL", "NULL"}}));
  o = run(b.s, "SELECT 1 + COUNT(*), SUM(i) / COUNT(i), SUM(i) * 2 FROM t");
  EXPECT_EQ(rows_of(o), (Rows{{"6", "10.5000", "84"}}));
  EXPECT_EQ(shapes(o),
            (Shapes{{f::longlong, bin, 22, 0, nn | binflag | num}, {f::newdecimal, bin, 38, 4, binflag | num}, {f::newdecimal, bin, 34, 0, binflag | num}}));
  EXPECT_EQ(b.rows("SELECT SUM(DISTINCT i), AVG(DISTINCT u), COUNT(DISTINCT g, s) FROM t"), (Rows{{"42", "3.0000", "3"}}));
  EXPECT_EQ(b.rows("SELECT COUNT(DISTINCT k), COUNT(DISTINCT c) FROM v"), (Rows{{"2", "2"}}));
  o = run(b.s, "SELECT SUM(k > 1), COUNT(n = 2), SUM(-n), AVG(n * 1.5) FROM v");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "3", "-6", "3.00000"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::newdecimal, bin, 24, 0, binflag | num},
                               {f::longlong, bin, 21, 0, nn | binflag | num},
                               {f::newdecimal, bin, 33, 0, binflag | num},
                               {f::newdecimal, bin, 18, 5, binflag | num}}));
  o = run(b.s, "SELECT SUM(9223372036854775807), SUM(18446744073709551615) FROM v");
  EXPECT_EQ(rows_of(o), (Rows{{"27670116110564327421", "55340232221128654845"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::newdecimal, bin, 42, 0, binflag | num}, {f::newdecimal, bin, 43, 0, binflag | num}}));
  // Strings summed as numbers, a warning for each that is not one, row by
  // row.
  o = run(b.s, "SELECT SUM(c), SUM(s) FROM v");
  EXPECT_EQ(rows_of(o), (Rows{{"0", "0"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::double_, bin, 23, 31, binflag | num}, {f::double_, bin, 23, 31, binflag | num}}));
  b.expect_warnings({{"Warning", 1292, "Truncated incorrect DOUBLE value: 'a'"},
                     {"Warning", 1292, "Truncated incorrect DOUBLE value: 'x'"},
                     {"Warning", 1292, "Truncated incorrect DOUBLE value: 'a'"},
                     {"Warning", 1292, "Truncated incorrect DOUBLE value: 'y'"},
                     {"Warning", 1292, "Truncated incorrect DOUBLE value: 'b'"},
                     {"Warning", 1292, "Truncated incorrect DOUBLE value: 'z'"}});
  EXPECT_EQ(b.rows("SELECT SUM('1x') FROM v"), (Rows{{"3"}}));
  EXPECT_EQ(b.s.warnings().size(), 3u);
  // BIT_AND, BIT_OR and BIT_XOR as unsigned BIGINTs; STD and VARIANCE over
  // the population, as DOUBLE.
  o = run(b.s, "SELECT BIT_AND(i), BIT_OR(i), BIT_XOR(i), STD(i), VARIANCE(i) FROM t");
  EXPECT_EQ(rows_of(o), (Rows{{"0", "31", "28", "5.766281297335398", "33.25"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::longlong, bin, 21, 0, nn | uns | binflag | num},
                               {f::longlong, bin, 21, 0, nn | uns | binflag | num},
                               {f::longlong, bin, 21, 0, nn | uns | binflag | num},
                               {f::double_, bin, 23, 31, binflag | num},
                               {f::double_, bin, 23, 31, binflag | num}}));
  // Without a table: one row.
  EXPECT_EQ(b.rows("SELECT COUNT(*)"), (Rows{{"1"}}));
  o = run(b.s, "SELECT SUM(1), MIN(2), AVG(3)");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "2", "3.0000"}}));
  EXPECT_EQ(shapes(o),
            (Shapes{{f::newdecimal, bin, 24, 0, binflag | num}, {f::longlong, bin, 2, 0, binflag | num}, {f::newdecimal, bin, 7, 4, binflag | num}}));
  // HAVING without GROUP BY filters the one group.
  EXPECT_TRUE(b.rows("SELECT COUNT(*) FROM v HAVING COUNT(*) > 5").empty());
  // A column beside an aggregate needs WHERE to fix it, and then is nullable.
  o = run(b.s, "SELECT COUNT(*), k FROM v WHERE k = 1");
  EXPECT_EQ(rows_of(o), (Rows{{"2", "1"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::longlong, bin, 21, 0, nn | binflag | num}, {f::long_, bin, 11, 0, nodef | num}}));
}

// GROUP BY: groups in the order they are first met, NULL a group of its
// own; a column, an alias or a position; HAVING over the groups (an alias,
// a grouped column or an aggregate); ORDER BY and LIMIT after.
TEST(ExecSessionGroups, GroupBy) {
  GroupBed b;
  auto o = run(b.s, "SELECT g, COUNT(*), SUM(i) FROM t GROUP BY g");
  EXPECT_EQ(rows_of(o), (Rows{{"a", "2", "30"}, {"b", "2", "5"}, {"NULL", "1", "7"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::var_string, ai, 40, 0, group_flag}, {f::longlong, bin, 21, 0, nn | num}, {f::newdecimal, bin, 33, 0, num}}));
  EXPECT_EQ(b.rows("SELECT g, COUNT(*) AS n FROM t GROUP BY g ORDER BY n DESC, g"), (Rows{{"a", "2"}, {"b", "2"}, {"NULL", "1"}}));
  EXPECT_EQ(b.rows("SELECT g, SUM(i) FROM t GROUP BY g HAVING SUM(i) > 10"), (Rows{{"a", "30"}}));
  EXPECT_EQ(b.rows("SELECT g, SUM(i) s FROM t GROUP BY g HAVING s > 10"), (Rows{{"a", "30"}}));
  EXPECT_EQ(b.rows("SELECT COUNT(*) FROM t GROUP BY g"), (Rows{{"2"}, {"2"}, {"1"}}));
  EXPECT_EQ(b.rows("SELECT g FROM t GROUP BY 1"), (Rows{{"a"}, {"b"}, {"NULL"}}));
  EXPECT_EQ(b.rows("SELECT g AS k, COUNT(*) FROM t GROUP BY k"), (Rows{{"a", "2"}, {"b", "2"}, {"NULL", "1"}}));
  EXPECT_EQ(b.rows("SELECT SUM(id) FROM t GROUP BY g ORDER BY g"), (Rows{{"5"}, {"3"}, {"7"}}));
  EXPECT_EQ(b.rows("SELECT k FROM v GROUP BY k HAVING COUNT(*) > 1"), (Rows{{"1"}}));
  EXPECT_EQ(b.rows("SELECT k AS x FROM v GROUP BY k HAVING x > 1"), (Rows{{"2"}}));
  EXPECT_EQ(b.rows("SELECT k FROM v GROUP BY k HAVING k > 1"), (Rows{{"2"}}));
  EXPECT_EQ(b.rows("SELECT COUNT(*) AS cnt FROM v GROUP BY k HAVING cnt > 1"), (Rows{{"2"}}));
  EXPECT_EQ(b.rows("SELECT k FROM v GROUP BY k ORDER BY SUM(n) DESC"), (Rows{{"1"}, {"2"}}));
  EXPECT_EQ(b.rows("SELECT COUNT(*) FROM v GROUP BY k HAVING SUM(n) > 0 ORDER BY k DESC LIMIT 1"), (Rows{{"1"}}));
  EXPECT_EQ(b.rows("SELECT k, COUNT(*) FROM v GROUP BY k ORDER BY COUNT(*) DESC, k LIMIT 1 OFFSET 1"), (Rows{{"2", "1"}}));
  EXPECT_EQ(b.rows("SELECT k + 1 FROM v GROUP BY k + 1"), (Rows{{"2"}, {"3"}}));
  EXPECT_EQ(b.rows("SELECT k * 2 FROM v GROUP BY k"), (Rows{{"2"}, {"4"}}));
  EXPECT_EQ(b.rows("SELECT 1 FROM v GROUP BY k"), (Rows{{"1"}, {"1"}}));
  EXPECT_EQ(b.rows("SELECT k FROM v GROUP BY K"), (Rows{{"1"}, {"2"}}));
  EXPECT_EQ(b.rows("SELECT v.k FROM v GROUP BY k"), (Rows{{"1"}, {"2"}}));
  EXPECT_EQ(b.rows("SELECT k FROM v GROUP BY v.k"), (Rows{{"1"}, {"2"}}));
  EXPECT_EQ(b.rows("SELECT MAX(n), MIN(n) FROM v GROUP BY k ORDER BY MAX(n)"), (Rows{{"2", "1"}, {"3", "3"}}));
  EXPECT_TRUE(b.rows("SELECT MIN(c), MAX(c) FROM v WHERE id > 99 GROUP BY k").empty());
  // A column fixed by the group: the primary key, or WHERE.
  EXPECT_EQ(b.rows("SELECT k, c FROM v GROUP BY id"), (Rows{{"1", "a"}, {"1", "a"}, {"2", "b"}}));
  EXPECT_EQ(b.rows("SELECT n, COUNT(*) FROM v WHERE n = 2 GROUP BY k"), (Rows{{"2", "1"}}));
  // SELECT DISTINCT: each row once, through a temporary table as GROUP BY
  // is, unless the list holds a whole key.
  EXPECT_EQ(b.rows("SELECT DISTINCT k FROM v"), (Rows{{"1"}, {"2"}}));
  o = run(b.s, "SELECT DISTINCT c, k FROM v");
  EXPECT_EQ(rows_of(o), (Rows{{"a", "1"}, {"b", "2"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::string, ai, 12, 0, group_flag}, {f::long_, bin, 11, 0, nn | nodef | num}}));
  EXPECT_EQ(b.rows("SELECT DISTINCT c FROM v ORDER BY c DESC"), (Rows{{"b"}, {"a"}}));
  o = run(b.s, "SELECT DISTINCT k + 1 FROM v");
  EXPECT_EQ(rows_of(o), (Rows{{"2"}, {"3"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::longlong, bin, 12, 0, nn | num}}));
  o = run(b.s, "SELECT DISTINCT * FROM w");
  EXPECT_EQ(rows_of(o).size(), 2u);
  EXPECT_EQ(shapes(o)[2], (Shape{f::var_string, ai, 12, 0, 0}));
}

// What a grouped result's columns report: MySQL groups through a temporary
// table, so a bare column, a bare COUNT, SUM, MIN or MAX, or an expression
// over columns is reported as that table's field (a number without BINARY,
// a string with 0 decimals, MIN and MAX of a column as the column less NOT
// NULL and its keys, a nullable GROUP BY column with GROUP_FLAG), while a
// constant, AVG and an expression over aggregates keep their own. A group
// WHERE fixes, or a DISTINCT aggregate, needs no such table.
TEST(ExecSessionGroups, GroupedColumns) {
  GroupBed b;
  auto o = run(b.s, "SELECT k, n, d, f, c, s, z, bs, id FROM v GROUP BY id");
  EXPECT_EQ(shapes(o), (Shapes{{f::long_, bin, 11, 0, nn | nodef | num},
                               {f::long_, bin, 11, 0, num},
                               {f::newdecimal, bin, 8, 2, num},
                               {f::double_, bin, 22, 31, num},
                               {f::string, ai, 12, 0, 0},
                               {f::var_string, ai, 20, 0, nn},
                               {f::long_, bin, 4, 0, uns | zf | num},
                               {f::var_string, bin, 4, 0, binflag},
                               {f::long_, bin, 11, 0, nn | pri | nodef | num | part}}));
  EXPECT_EQ(rows_of(o).front(), (std::vector<std::string>{"1", "1", "1.00", "1", "a", "x", "0001", "p", "1"}));
  o = run(b.s, "SELECT k, k + 1, 'lit', 5, 2.5, NULL, COUNT(*), SUM(n), MIN(c), MAX(s), MIN(z), MAX(bs), AVG(d) FROM v GROUP BY k");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "2", "lit", "5", "2.5", "NULL", "2", "3", "a", "y", "0001", "q", "1.500000"},
                              {"2", "3", "lit", "5", "2.5", "NULL", "1", "3", "b", "z", "0003", "r", "3.000000"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::long_, bin, 11, 0, nn | nodef | num},
                               {f::longlong, bin, 12, 0, nn | num},
                               {f::var_string, ai, 12, 31, nn},
                               {f::longlong, bin, 2, 0, nn | binflag | num},
                               {f::newdecimal, bin, 4, 1, nn | binflag | num},
                               {f::null, bin, 0, 0, binflag | num},
                               {f::longlong, bin, 21, 0, nn | num},
                               {f::newdecimal, bin, 33, 0, num},
                               {f::string, ai, 12, 0, 0},
                               {f::var_string, ai, 20, 0, 0},
                               {f::long_, bin, 4, 0, uns | zf | num},
                               {f::var_string, bin, 4, 0, binflag},
                               {f::newdecimal, bin, 12, 6, binflag | num}}));
  EXPECT_EQ(b.shapes_of("SELECT k AS kk, MIN(tx), COUNT(n) FROM v GROUP BY kk"),
            (Shapes{{f::long_, bin, 11, 0, nn | nodef | num}, {f::blob, ai, 262140, 0, blob}, {f::longlong, bin, 21, 0, nn | num}}));
  EXPECT_EQ(b.shapes_of("SELECT a, b, c FROM w GROUP BY a, b, c"),
            (Shapes{{f::string, ai, 8, 0, nn | nodef}, {f::var_string, ai, 12, 0, group_flag}, {f::var_string, ai, 12, 0, nn | nodef}}));
  EXPECT_EQ(b.shapes_of("SELECT e FROM w GROUP BY e"), (Shapes{{f::blob, ai, 262140, 0, blob | group_flag}}));
  EXPECT_EQ(b.shapes_of("SELECT b AS bb, a FROM w GROUP BY bb, a"), (Shapes{{f::var_string, ai, 12, 0, group_flag}, {f::string, ai, 8, 0, nn | nodef}}));
  EXPECT_EQ(b.shapes_of("SELECT MIN(id), MAX(a), MIN(c), MIN(e), MIN(i + 1), MAX(d), MIN(x) FROM w GROUP BY b"),
            (Shapes{{f::long_, bin, 11, 0, nodef | num},
                    {f::string, ai, 8, 0, nodef},
                    {f::var_string, ai, 12, 0, nodef},
                    {f::blob, ai, 262140, 0, blob},
                    {f::longlong, bin, 12, 0, num},
                    {f::newdecimal, bin, 6, 1, nodef | num},
                    {f::double_, bin, 22, 31, num}}));
  o = run(b.s, "SELECT COUNT(*) + 0.5, -COUNT(*), COUNT(*) * k FROM v GROUP BY k");
  EXPECT_EQ(rows_of(o), (Rows{{"2.5", "-2", "2"}, {"1.5", "-1", "2"}}));
  EXPECT_EQ(
      shapes(o),
      (Shapes{{f::newdecimal, bin, 24, 1, nn | binflag | num}, {f::longlong, bin, 21, 0, nn | binflag | num}, {f::longlong, bin, 31, 0, nn | binflag | num}}));
  EXPECT_EQ(b.shapes_of("SELECT 1 + 1, a, 'x' FROM w GROUP BY a"),
            (Shapes{{f::longlong, bin, 3, 0, nn | binflag | num}, {f::string, ai, 8, 0, nn | nodef}, {f::var_string, ai, 4, 31, nn}}));
  o = run(b.s, "SELECT MIN(1), MAX('s'), SUM(2), COUNT(3) FROM w GROUP BY a");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "s", "4", "2"}}));
  EXPECT_EQ(shapes(o),
            (Shapes{{f::longlong, bin, 2, 0, num}, {f::var_string, ai, 4, 0, 0}, {f::newdecimal, bin, 24, 0, num}, {f::longlong, bin, 21, 0, nn | num}}));
  // No temporary table: a group WHERE fixes, or a DISTINCT aggregate.
  EXPECT_EQ(b.shapes_of("SELECT a, COUNT(*) FROM w WHERE a = 'p' GROUP BY a"),
            (Shapes{{f::string, ai, 8, 0, nn | nodef}, {f::longlong, bin, 21, 0, nn | binflag | num}}));
  EXPECT_EQ(b.shapes_of("SELECT b, COUNT(*) FROM w WHERE b = 'q' GROUP BY b"),
            (Shapes{{f::var_string, ai, 12, 0, 0}, {f::longlong, bin, 21, 0, nn | binflag | num}}));
  EXPECT_EQ(b.shapes_of("SELECT a, COUNT(*) FROM w WHERE a = 'p' AND id > 0 GROUP BY a, b"),
            (Shapes{{f::string, ai, 8, 0, nn | nodef}, {f::longlong, bin, 21, 0, nn | num}}));
  o = run(b.s, "SELECT a, b, i + 1, COUNT(DISTINCT i), SUM(i), MIN(i), MAX(b) FROM w GROUP BY a, b, i + 1");
  EXPECT_EQ(rows_of(o), (Rows{{"p", "q", "2", "1", "1", "1", "q"}, {"p", "q", "3", "1", "2", "2", "q"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::string, ai, 8, 0, nn | nodef},
                               {f::var_string, ai, 12, 0, 0},
                               {f::longlong, bin, 12, 0, binflag | num},
                               {f::longlong, bin, 21, 0, nn | binflag | num},
                               {f::newdecimal, bin, 33, 0, binflag | num},
                               {f::long_, bin, 11, 0, binflag | num},
                               {f::var_string, ai, 12, 31, 0}}));
}

// The refusals: ONLY_FULL_GROUP_BY (1055, 1140), an aggregate where none
// may be (1111, 1056), a name or position not there (1054); without
// ONLY_FULL_GROUP_BY a column not grouped takes its group's first row.
TEST(ExecSessionGroups, Refusals) {
  GroupBed b;
  const std::string incompatible = "; this is incompatible with sql_mode=only_full_group_by";
  b.expect_refused("SELECT g, i FROM t GROUP BY g", 1055, "42000",
                   "Expression #2 of SELECT list is not in GROUP BY clause and contains nonaggregated column 'pt.t.i' which is not functionally dependent on "
                   "columns in GROUP BY clause" +
                       incompatible);
  b.expect_refused("SELECT k FROM v GROUP BY k ORDER BY n", 1055, "42000",
                   "Expression #1 of ORDER BY clause is not in GROUP BY clause and contains nonaggregated column 'pt.v.n' which is not functionally dependent "
                   "on columns in GROUP BY clause" +
                       incompatible);
  b.expect_refused("SELECT id, COUNT(*) FROM t", 1140, "42000",
                   "In aggregated query without GROUP BY, expression #1 of SELECT list contains nonaggregated column 'pt.t.id'" + incompatible);
  b.expect_refused("SELECT COUNT(*) FROM t WHERE COUNT(*) > 1", 1111, "HY000", "Invalid use of group function");
  b.expect_refused("SELECT COUNT(COUNT(*)) FROM v", 1111, "HY000", "Invalid use of group function");
  b.expect_refused("SELECT MAX(n) FROM v GROUP BY MAX(n)", 1056, "42000", "Can't group on 'MAX(n)'");
  b.expect_refused("SELECT k FROM v GROUP BY k HAVING n > 1", 1054, "42S22", "Unknown column 'n' in 'having clause'");
  b.expect_refused("SELECT SUM(n) FROM v GROUP BY 2", 1054, "42S22", "Unknown column '2' in 'group statement'");
  b.expect_refused("SELECT k FROM v GROUP BY nope", 1054, "42S22", "Unknown column 'nope' in 'group statement'");
  b.expect_refused("SELECT k FROM v GROUP BY k WITH ROLLUP", 1235, "42000", "This version of Kvasir doesn't yet support 'WITH ROLLUP'");
  b.expect_refused("SELECT GROUP_CONCAT(c) FROM v", 1235, "42000", "This version of Kvasir doesn't yet support 'GROUP_CONCAT'");
  run(b.s, "SET SESSION sql_mode = ''");
  EXPECT_EQ(b.rows("SELECT k, n FROM v GROUP BY k ORDER BY k"), (Rows{{"1", "1"}, {"2", "3"}}));
  auto o = run(b.s, "SELECT id, COUNT(*) FROM v");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "3"}}));
  EXPECT_EQ(shapes(o).front(), (Shape{f::long_, bin, 11, 0, pri | nodef | num | part}));
  b.expect_refused("SELECT k FROM v GROUP BY k HAVING n > 1", 1054, "42S22", "Unknown column 'n' in 'having clause'");
  b.expect_refused("SELECT MAX(n) FROM v WHERE MAX(n) > 1", 1111, "HY000", "Invalid use of group function");
}

// The flow control functions over a table's columns, observed on MySQL
// 8.4.13, 2026-11-17: a column counts as an expression over it would (a
// TEXT's length once more in characters); integers keep the widest column
// type, one wider where signed meets unsigned; a string result is a CHAR
// only when every string is, a BLOB when any is.
TEST(ExecSessionFunctions, FlowControlOverColumns) {
  TableBed b;
  run(b.s,
      "CREATE TABLE f (i INT, u INT UNSIGNED NOT NULL, b BIGINT, d DECIMAL(6,2), v VARCHAR(10), c CHAR(3) COLLATE utf8mb4_bin, t TINYINT, x TEXT, "
      "db DOUBLE, vb VARBINARY(5))");
  run(b.s, "INSERT INTO f VALUES (1, 2, 3, 4.5, 'abc', 'x', 7, 'tt', 1.25, 'zz')");
  auto o = run(b.s,
               "SELECT IFNULL(i,u), IFNULL(u,u), IFNULL(t,t), IFNULL(t,i), IFNULL(i,b), IFNULL(d,i), IFNULL(v,c), IFNULL(v,x), IFNULL(v,i), "
               "COALESCE(i,v,d), IFNULL(db,d), IFNULL(vb,v), IFNULL(u,-1), IFNULL(c,c) FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "2", "7", "7", "1", "4.50", "abc", "abc", "abc", "1", "1.25", "zz", "2", "x"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::longlong, bin, 11, 0, nn | binflag | num},
                               {f::long_, bin, 10, 0, nn | uns | binflag | num},
                               {f::tiny, bin, 4, 0, binflag | num},
                               {f::long_, bin, 11, 0, binflag | num},
                               {f::longlong, bin, 20, 0, binflag | num},
                               {f::newdecimal, bin, 14, 2, binflag | num},
                               {f::var_string, ai, 40, 31, binflag},
                               {f::blob, ai, 1048560, 31, 0},
                               {f::var_string, ai, 44, 31, 0},
                               {f::var_string, ai, 44, 31, 0},
                               {f::double_, bin, 23, 31, binflag | num},
                               {f::var_string, bin, 40, 31, binflag},
                               {f::longlong, bin, 10, 0, nn | binflag | num},
                               {f::string, ai, 12, 31, binflag}}));
  o = run(b.s,
          "SELECT IFNULL(db,'a'), IFNULL(c,'a'), IFNULL(x,x), IFNULL(i,'a'), IFNULL(d,'a'), IFNULL(c,v), IFNULL(v,_binary'a'), IFNULL(t,1), "
          "IFNULL(i,1.5), IFNULL(u,1.5), IFNULL(b,1.5), IFNULL(t,2.5), IFNULL(i,1e0), IFNULL(c,1), IFNULL(NULL,v), IFNULL(NULL,t) FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"1.25", "x", "tt", "1", "4.50", "x", "abc", "7", "1.0", "2.0", "3.0", "7.0", "1", "x", "abc", "7"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::var_string, ai, 88, 31, nn},
                               {f::var_string, ai, 12, 31, nn | binflag},
                               {f::blob, ai, 1048560, 31, 0},
                               {f::var_string, ai, 44, 31, nn},
                               {f::var_string, ai, 32, 31, nn},
                               {f::var_string, ai, 40, 31, binflag},
                               {f::var_string, ai, 40, 31, nn},
                               {f::longlong, bin, 4, 0, nn | binflag | num},
                               {f::newdecimal, bin, 13, 1, nn | binflag | num},
                               {f::newdecimal, bin, 13, 1, nn | binflag | num},
                               {f::newdecimal, bin, 22, 1, nn | binflag | num},
                               {f::newdecimal, bin, 6, 1, nn | binflag | num},
                               {f::double_, bin, 23, 31, nn | binflag | num},
                               {f::string, ai, 12, 31, nn | binflag},
                               {f::var_string, ai, 40, 31, 0},
                               {f::tiny, bin, 4, 0, binflag | num}}));
  // NULLIF reports its first argument, never NOT NULL.
  o = run(b.s, "SELECT NULLIF(i,1), NULLIF(v,'x'), NULLIF(u,2), NULLIF(t,1), NULLIF(c,1), NULLIF(x,1), NULLIF(db,1) FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"NULL", "abc", "NULL", "7", "x", "tt", "1.25"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::long_, bin, 11, 0, binflag | num},
                               {f::var_string, ai, 40, 31, 0},
                               {f::long_, bin, 10, 0, uns | binflag | num},
                               {f::tiny, bin, 4, 0, binflag | num},
                               {f::string, ai, 12, 31, binflag},
                               {f::blob, ai, 1048560, 31, 0},
                               {f::double_, bin, 22, 31, binflag | num}}));
  o = run(b.s,
          "SELECT IF(i,v,c), IF(1,t,t), CASE i WHEN 1 THEN u ELSE t END, CASE WHEN i THEN x END, IF(1, i, d), CASE WHEN 1 THEN i ELSE d END, "
          "COALESCE(i, d), IFNULL(d, 1.123) FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"abc", "7", "2", "tt", "1", "1", "1.00", "4.500"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::var_string, ai, 40, 31, binflag},
                               {f::tiny, bin, 4, 0, binflag | num},
                               {f::longlong, bin, 10, 0, binflag | num},
                               {f::blob, ai, 1048560, 31, 0},
                               {f::newdecimal, bin, 14, 2, binflag | num},
                               {f::newdecimal, bin, 14, 2, binflag | num},
                               {f::newdecimal, bin, 14, 2, binflag | num},
                               {f::newdecimal, bin, 9, 3, nn | binflag | num}}));
}

// CONCAT, CONCAT_WS, LENGTH and CHAR_LENGTH over columns, observed on MySQL
// 8.4.13, 2026-11-17: a column counts by its own length; past 65535 bytes
// the result is a MEDIUM or LONG BLOB, whose text length is counted in
// characters once more, and at most max_allowed_packet.
TEST(ExecSessionFunctions, ConcatAndLengthOverColumns) {
  TableBed b;
  run(b.s,
      "CREATE TABLE f (i INT, u INT UNSIGNED NOT NULL, b BIGINT, d DECIMAL(6,2), v VARCHAR(10), c CHAR(3) COLLATE utf8mb4_bin, t TINYINT, x TEXT, "
      "db DOUBLE, vb VARBINARY(5))");
  run(b.s, "INSERT INTO f VALUES (1, 2, 3, 4.5, 'abc', 'x', 7, 'tt', 1.25, 'zz')");
  auto o = run(b.s,
               "SELECT CONCAT(v,c), CONCAT(i,v), CONCAT(u), CONCAT(x,'a'), CONCAT(vb,v), CONCAT(c), CONCAT(d), CONCAT(db), CONCAT_WS(v,c,i), LENGTH(v), "
               "CHAR_LENGTH(x), LENGTH(u), CONCAT(v, v, v, v) FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"abcx", "1abc", "2", "tta", "zzabc", "x", "4.50", "1.25", "xabc1", "3", "2", "1", "abcabcabcabc"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::var_string, ai, 52, 31, binflag},
                               {f::var_string, ai, 84, 31, 0},
                               {f::var_string, ai, 40, 31, 0},
                               {f::medium_blob, ai, 1048576, 31, 0},
                               {f::var_string, bin, 45, 31, binflag},
                               {f::var_string, ai, 12, 31, binflag},
                               {f::var_string, ai, 32, 31, 0},
                               {f::var_string, ai, 88, 31, 0},
                               {f::var_string, ai, 96, 31, binflag},
                               {f::longlong, bin, 10, 0, binflag | num},
                               {f::longlong, bin, 10, 0, binflag | num},
                               {f::longlong, bin, 10, 0, nn | binflag | num},
                               {f::var_string, ai, 160, 31, 0}}));
  run(b.s, "CREATE TABLE g (a VARCHAR(100), b VARCHAR(200), m VARCHAR(8000), t TINYTEXT, bl BLOB, vb VARBINARY(300), mt MEDIUMTEXT, x TEXT)");
  o = run(b.s,
          "SELECT CONCAT(a,a), CONCAT(b,b), CONCAT(b,b,b), CONCAT(m), CONCAT(m,m), CONCAT(m,m,m,m,m), CONCAT(t), CONCAT(bl), CONCAT(x), "
          "CONCAT(vb, vb), CONCAT(mt), CONCAT(x,x,x,x,x), CONCAT(vb,vb,vb,vb,vb,vb,vb,vb) FROM g");
  EXPECT_EQ(rows_of(o), Rows{});
  EXPECT_EQ(shapes(o), (Shapes{{f::var_string, ai, 800, 31, 0},
                               {f::var_string, ai, 1600, 31, 0},
                               {f::var_string, ai, 2400, 31, 0},
                               {f::var_string, ai, 32000, 31, 0},
                               {f::var_string, ai, 64000, 31, 0},
                               {f::medium_blob, ai, 640000, 31, 0},
                               {f::var_string, ai, 1020, 31, 0},
                               {f::var_string, bin, 65535, 31, binflag},
                               {f::medium_blob, ai, 1048560, 31, 0},
                               {f::var_string, bin, 600, 31, binflag},
                               {f::long_blob, ai, 67108864, 31, 0},
                               {f::medium_blob, ai, 5242800, 31, 0},
                               {f::var_string, bin, 2400, 31, binflag}}));
}

// TRIM, REPLACE, LOCATE, LPAD, RPAD, REPEAT and REVERSE over columns,
// observed on MySQL 8.4.13, 2026-11-17: the first argument's collation; a
// length or count from a column makes a LONG BLOB; REPLACE as long as the
// string times one less than the replacement's length (at least once).
TEST(ExecSessionFunctions, MoreStringFunctionsOverColumns) {
  TableBed b;
  run(b.s, "CREATE TABLE f (i INT, v VARCHAR(10), c CHAR(3) COLLATE utf8mb4_bin, x TEXT, vb VARBINARY(5))");
  run(b.s, "INSERT INTO f VALUES (1, 'abc', 'x', 'tt', 'zz')");
  auto o = run(b.s,
               "SELECT LPAD(v, i, 'x'), REPEAT(v, i), TRIM(c), REPLACE(v, 'b', c), REVERSE(x), LOCATE(c, v), RPAD(c, 4, v), TRIM(BOTH c FROM v), "
               "REPEAT(vb, 2), LPAD(v, 3, vb) FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"a", "abc", "x", "axc", "tt", "0", "xabc", "abc", "zzzz", "abc"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::long_blob, ai, 268435456, 31, 0},
                               {f::long_blob, ai, 268435456, 31, 0},
                               {f::var_string, ai, 12, 31, binflag},
                               {f::var_string, ai, 80, 31, 0},
                               {f::medium_blob, ai, 1048560, 31, 0},
                               {f::longlong, bin, 11, 0, binflag | num},
                               {f::var_string, ai, 16, 31, binflag},
                               {f::var_string, ai, 40, 31, 0},
                               {f::var_string, bin, 10, 31, binflag},
                               {f::var_string, ai, 12, 31, 0}}));
  o = run(b.s,
          "SELECT REPLACE(v,'b',v), REPLACE(v,v,'xyz'), REPLACE(v,'bc',c), REPLACE(c,'b',v), REPLACE('abc','b',c), REPLACE(v,'b',''), "
          "REPLACE(v, c, 'x') FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"aabcc", "xyz", "ax", "x", "axc", "ac", "abc"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::var_string, ai, 360, 31, 0},
                               {f::var_string, ai, 80, 31, 0},
                               {f::var_string, ai, 80, 31, 0},
                               {f::var_string, ai, 108, 31, binflag},
                               {f::var_string, ai, 24, 31, 0},
                               {f::var_string, ai, 40, 31, 0},
                               {f::var_string, ai, 40, 31, 0}}));
}

// ABS, FLOOR, ROUND, TRUNCATE, SIGN and CEILING over columns, observed on
// MySQL 8.4.13, 2026-11-17: ABS keeps an integer column's length as a
// BIGINT; ROUND with a decimals count that is not a constant keeps the
// column's scale.
TEST(ExecSessionFunctions, NumberFunctionsOverColumns) {
  TableBed b;
  run(b.s, "CREATE TABLE f (i INT, u INT UNSIGNED NOT NULL, d DECIMAL(6,2), t TINYINT, db DOUBLE)");
  run(b.s, "INSERT INTO f VALUES (1, 2, 4.5, 7, 1.25)");
  auto o = run(b.s,
               "SELECT ABS(i), ABS(u), ABS(d), ABS(t), FLOOR(d), FLOOR(i), FLOOR(db), ROUND(d), ROUND(d, 1), ROUND(d, i), ROUND(i, i), "
               "TRUNCATE(d, 1), SIGN(d), CEILING(u), ROUND(i, -1), ROUND(u) FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"1", "2", "4.50", "7", "4", "1", "1", "5", "4.5", "4.50", "1", "4.5", "1", "2", "0", "2"}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::longlong, bin, 11, 0, binflag | num},
                               {f::longlong, bin, 10, 0, nn | uns | binflag | num},
                               {f::newdecimal, bin, 8, 2, binflag | num},
                               {f::longlong, bin, 4, 0, binflag | num},
                               {f::longlong, bin, 21, 0, binflag | num},
                               {f::longlong, bin, 21, 0, binflag | num},
                               {f::double_, bin, 23, 31, binflag | num},
                               {f::newdecimal, bin, 6, 0, binflag | num},
                               {f::newdecimal, bin, 8, 1, binflag | num},
                               {f::newdecimal, bin, 8, 2, binflag | num},
                               {f::longlong, bin, 21, 0, binflag | num},
                               {f::newdecimal, bin, 7, 1, binflag | num},
                               {f::longlong, bin, 21, 0, binflag | num},
                               {f::longlong, bin, 21, 0, nn | uns | binflag | num},
                               {f::longlong, bin, 21, 0, binflag | num},
                               {f::longlong, bin, 21, 0, nn | uns | binflag | num}}));
}

// SUBSTRING, LEFT and RIGHT over columns, observed on MySQL 8.4.13,
// 2026-11-17: a position or length from a column takes nothing from the
// length; a short enough piece of a TEXT is a VARCHAR.
TEST(ExecSessionFunctions, SubstringOverColumns) {
  TableBed b;
  run(b.s, "CREATE TABLE f (i INT, d DECIMAL(6,2), v VARCHAR(10), c CHAR(3) COLLATE utf8mb4_bin, x TEXT, vb VARBINARY(5))");
  run(b.s, "INSERT INTO f VALUES (1, 4.5, 'abc', 'x', 'tt', 'zz')");
  auto o = run(b.s,
               "SELECT SUBSTRING(v,2), SUBSTRING(v,i), SUBSTRING(v,1,i), LEFT(v,2), RIGHT(c,1), LEFT(x,3), SUBSTRING(i,1,1), LEFT(vb,1), "
               "SUBSTRING(v,-1), LEFT(d, 2) FROM f");
  EXPECT_EQ(rows_of(o), (Rows{{"bc", "abc", "a", "ab", "x", "tt", "1", "z", "c", "4."}}));
  EXPECT_EQ(shapes(o), (Shapes{{f::var_string, ai, 36, 31, 0},
                               {f::var_string, ai, 40, 31, 0},
                               {f::var_string, ai, 40, 31, 0},
                               {f::var_string, ai, 8, 31, 0},
                               {f::var_string, ai, 4, 31, binflag},
                               {f::var_string, ai, 12, 31, 0},
                               {f::var_string, ai, 4, 31, 0},
                               {f::var_string, bin, 1, 31, binflag},
                               {f::var_string, ai, 4, 31, 0},
                               {f::var_string, ai, 8, 31, 0}}));
}

// information_schema's views Kvasir does not build yet are refused as not
// yet supported, in any case, where MySQL would answer them (the design,
// plan 1c); a name MySQL does not have is MySQL's 1109.
TEST(ExecSessionInformationSchema, ViewsNotBuiltAreRefused) {
  TableBed b;
  b.expect_refused("SELECT * FROM information_schema.VIEWS", 1235, "42000", "This version of Kvasir doesn't yet support 'information_schema.VIEWS'");
  b.expect_refused("SELECT * FROM INFORMATION_SCHEMA.routines", 1235, "42000", "This version of Kvasir doesn't yet support 'information_schema.ROUTINES'");
  b.expect_refused("SELECT * FROM information_schema.NOPE", 1109, "42S02", "Unknown table 'NOPE' in information_schema");
}

// A type Kvasir does not build yet is refused as not yet supported, where
// MySQL would create the table (the date and time types wait on step 2).
TEST(ExecSessionDdl, TypesNotBuiltAreRefused) {
  TableBed b;
  b.expect_refused("CREATE TABLE d (a DATE)", 1235, "42000", "This version of Kvasir doesn't yet support 'the DATE type'");
}

// Index and column options Kvasir does not build yet are refused as not yet
// supported, where MySQL would take them.
TEST(ExecSessionDdl, OptionsNotBuiltAreRefused) {
  TableBed b;
  b.expect_refused("CREATE TABLE o1 (a INT, KEY (a) INVISIBLE)", 1235, "42000", "This version of Kvasir doesn't yet support 'index option INVISIBLE'");
  b.expect_refused("CREATE TABLE o2 (a INT ON UPDATE 1)", 1235, "42000", "This version of Kvasir doesn't yet support 'ON UPDATE'");
  b.expect_refused("CREATE TABLE o3 (a INT VISIBLE)", 1235, "42000", "This version of Kvasir doesn't yet support 'column attribute VISIBLE'");
}

// ASCII is CHARACTER SET latin1 and UNICODE is ucs2 (the manual's "String
// Data Type Syntax"; MySQL creates both), character sets a column cannot yet
// hold.
TEST(ExecSessionDdl, CharacterSetsNotBuiltAreRefused) {
  TableBed b;
  b.expect_refused("CREATE TABLE c1 (a CHAR(1) ASCII)", 1235, "42000", "This version of Kvasir doesn't yet support 'character set latin1'");
  b.expect_refused("CREATE TABLE c2 (a CHAR(1) UNICODE)", 1235, "42000", "This version of Kvasir doesn't yet support 'character set ucs2'");
}

}  // namespace
}  // namespace kvasir::exec
