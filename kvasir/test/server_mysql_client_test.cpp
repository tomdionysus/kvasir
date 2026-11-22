//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A node on a loopback port, driven by a real MySQL client library as an
// application would: Connector/C++ 26.10.0's classic API over libmysqlclient
// 8.4.13, built from Oracle's source for the tests only (the maintainer,
// 2026-11-20 and 21). TLS first, caching_sha2_password's full path and then
// its fast path, text and prepared statements and their metadata, a rollback,
// and errors as MySQL 8.4 reports them.

#include <gtest/gtest.h>
#include <jdbc/cppconn/connection.h>
#include <jdbc/cppconn/datatype.h>
#include <jdbc/cppconn/exception.h>
#include <jdbc/cppconn/metadata.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/resultset_metadata.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/mysql_driver.h>
#include <openssl/bio.h>
#include <openssl/pem.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <tuple>

#include "dannan/identity/certificate.h"
#include "dannan/identity/signing.h"
#include "dannan/test/fixtures/ports.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "kvasir/server/accounts.h"
#include "kvasir/server/node.h"

namespace kvasir::server {
namespace {

// Connector/C++'s namespace; sql:: here is Kvasir's own parser's.
namespace jdbc = ::sql;

constexpr const char* password = "kvasir-client-test";

uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string key_pem(const dannan::identity::SigningKey& key) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
  PEM_write_bio_PrivateKey(bio.get(), key.native_handle(), nullptr, nullptr, 0, nullptr, nullptr);
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, static_cast<size_t>(size));
}

void write(const std::filesystem::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

dannan::identity::Argon2Costs cheap() {
  dannan::identity::Argon2Costs costs;
  costs.memory_kib = 8;
  costs.iterations = 1;
  costs.lanes = 1;
  return costs;
}

// One node serving on its own threads for the test's length, its certificate
// issued by a CA made for the test.
class MysqlClient : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto ca = dannan::identity::CertificateAuthority::create("kvasir client test ca", now_ms() - 60000);
    const auto key = dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::es256);
    const auto certificate = ca.issue(dannan::identity::NodeId::random(), key.public_key(), 1, now_ms() - 60000);
    write(_dir.path() / "node.crt", certificate.pem() + ca.certificate().pem());
    write(_dir.path() / "node.key", key_pem(key));

    NodeSettings settings;
    settings.host = "127.0.0.1";
    settings.port = dannan::test::free_port();
    settings.certificate = (_dir.path() / "node.crt").string();
    settings.key = (_dir.path() / "node.key").string();
    settings.ack_level = "ephemeral";
    const std::string text(password);
    settings.users = {"root:" + make_credential({reinterpret_cast<const uint8_t*>(text.data()), text.size()}, cheap())};
    _node = std::make_unique<Node>(settings);
    _serving = std::thread([this] { _node->run(2); });
  }

  void TearDown() override {
    _node->stop();
    _serving.join();
    _node.reset();
  }

  std::unique_ptr<jdbc::Connection> connect(const std::string& pass = password) {
    jdbc::ConnectOptionsMap options;
    options["hostName"] = jdbc::SQLString("tcp://127.0.0.1:" + std::to_string(_node->port()));
    options["userName"] = jdbc::SQLString("root");
    options["password"] = jdbc::SQLString(pass);
    options["OPT_SSL_MODE"] = jdbc::SSL_MODE_REQUIRED;
    return std::unique_ptr<jdbc::Connection>(jdbc::mysql::get_mysql_driver_instance()->connect(options));
  }

  // The SQLException `run` throws: its code, its SQLSTATE and its message.
  template <typename F>
  static std::tuple<int, std::string, std::string> refusal(F run) {
    try {
      run();
    } catch (const jdbc::SQLException& e) {
      return {e.getErrorCode(), e.getSQLState(), e.what()};
    }
    return {0, "", "nothing thrown"};
  }

 private:
  dannan::test::TempDir _dir;
  std::unique_ptr<Node> _node;
  std::thread _serving;
};

int64_t single_int(jdbc::Connection& c, const std::string& query) {
  std::unique_ptr<jdbc::Statement> statement(c.createStatement());
  std::unique_ptr<jdbc::ResultSet> rows(statement->executeQuery(query));
  EXPECT_TRUE(rows->next()) << query;
  return rows->getInt64(1);
}

// The first sign-in of an account takes caching_sha2_password's full path
// (the password sent over TLS, checked against its credential); the node
// then holds the account's SHA-256 verifier, and the second takes the fast
// path (a scramble). Manual, "Caching SHA-2 Pluggable Authentication".
TEST_F(MysqlClient, SignsInOverTlsByTheFullPathAndThenTheFastPath) {
  for (int i = 0; i < 2; ++i) {
    auto c = connect();
    EXPECT_EQ(single_int(*c, "SELECT 1 + 1"), 2) << "sign-in " << i;
  }
}

// ER_ACCESS_DENIED_ERROR, 1045, SQLSTATE 28000 (manual, "Server Error
// Message Reference").
TEST_F(MysqlClient, AWrongPasswordIsRefused) {
  const auto [code, state, message] = refusal([&] { connect("not the password"); });
  EXPECT_EQ(code, 1045) << message;
  EXPECT_EQ(state, "28000");
  EXPECT_EQ(message.rfind("Access denied for user 'root'@'", 0), 0u) << message;
}

TEST_F(MysqlClient, RunsTextStatementsAndReadsTheirMetadata) {
  auto c = connect();
  std::unique_ptr<jdbc::Statement> statement(c->createStatement());
  statement->execute("CREATE DATABASE kv_client");
  c->setSchema("kv_client");
  statement->execute("CREATE TABLE t (a INT PRIMARY KEY, b VARCHAR(10))");
  EXPECT_EQ(statement->executeUpdate("INSERT INTO t VALUES (1, 'one'), (2, NULL)"), 2);

  std::unique_ptr<jdbc::ResultSet> rows(statement->executeQuery("SELECT a, b FROM t ORDER BY a"));
  auto* meta = rows->getMetaData();
  ASSERT_EQ(meta->getColumnCount(), 2u);
  EXPECT_EQ(meta->getColumnName(1), "a");
  EXPECT_EQ(meta->getColumnTypeName(1), "INT");
  EXPECT_EQ(meta->getColumnName(2), "b");
  EXPECT_EQ(meta->getColumnTypeName(2), "VARCHAR");
  EXPECT_EQ(meta->getTableName(1), "t");
  EXPECT_EQ(meta->getSchemaName(1), "kv_client");
  ASSERT_TRUE(rows->next());
  EXPECT_EQ(rows->getInt(1), 1);
  EXPECT_EQ(rows->getString(2), "one");
  ASSERT_TRUE(rows->next());
  EXPECT_EQ(rows->getInt(1), 2);
  rows->getString(2);
  EXPECT_TRUE(rows->wasNull());
  EXPECT_FALSE(rows->next());
}

// Connector/C++'s prepared statements are the server's (COM_STMT_PREPARE
// and COM_STMT_EXECUTE, the binary protocol): parameters of each type, a
// NULL, and rows read back in binary. The statements are Connector/J's
// earlier probe's, which MySQL 8.4.13 answered so.
TEST_F(MysqlClient, RunsPreparedStatementsInTheBinaryProtocol) {
  auto c = connect();
  std::unique_ptr<jdbc::Statement> statement(c->createStatement());
  statement->execute("CREATE DATABASE kv_prep");
  c->setSchema("kv_prep");
  statement->execute("CREATE TABLE t (a INT PRIMARY KEY, b VARCHAR(10))");
  statement->execute("INSERT INTO t VALUES (1, 'x'), (2, 'zz'), (3, 'y')");

  std::unique_ptr<jdbc::PreparedStatement> select(c->prepareStatement("SELECT a, b FROM t WHERE a > ? AND b <> ? ORDER BY a"));
  select->setInt(1, 1);
  select->setString(2, "zz");
  std::unique_ptr<jdbc::ResultSet> rows(select->executeQuery());
  ASSERT_TRUE(rows->next());
  EXPECT_EQ(rows->getInt(1), 3);
  EXPECT_EQ(rows->getString(2), "y");
  EXPECT_FALSE(rows->next());

  std::unique_ptr<jdbc::PreparedStatement> insert(c->prepareStatement("INSERT INTO t VALUES (?, ?)"));
  insert->setInt(1, 9);
  insert->setNull(2, jdbc::DataType::VARCHAR);
  EXPECT_EQ(insert->executeUpdate(), 1);
  std::unique_ptr<jdbc::PreparedStatement> remove(c->prepareStatement("DELETE FROM t WHERE a = ?"));
  remove->setInt64(1, 9);
  EXPECT_EQ(remove->executeUpdate(), 1);

  std::unique_ptr<jdbc::PreparedStatement> values(c->prepareStatement("SELECT ?, ? + 1, CONCAT(?, 'x')"));
  values->setInt64(1, -5);
  values->setDouble(2, 2.5);
  values->setString(3, "ab");
  std::unique_ptr<jdbc::ResultSet> row(values->executeQuery());
  ASSERT_TRUE(row->next());
  EXPECT_EQ(row->getString(1), "-5");
  // Read as a double: the connector writes any binary DOUBLE as "%f" would.
  EXPECT_EQ(row->getDouble(2), 3.5);
  EXPECT_EQ(row->getString(3), "abx");
}

// DatabaseMetaData's getTables and getColumns, which Connector/C++ answers
// with its own prepared queries over information_schema (STRCMP, IF, UCASE,
// LOCATE, SUBSTRING): one row per table and per column, in JDBC's shape.
TEST_F(MysqlClient, ReadsTablesAndColumnsThroughDatabaseMetaData) {
  auto c = connect();
  std::unique_ptr<jdbc::Statement> statement(c->createStatement());
  statement->execute("CREATE DATABASE kv_meta");
  c->setSchema("kv_meta");
  statement->execute("CREATE TABLE t (a INT PRIMARY KEY, b VARCHAR(10))");
  auto* meta = c->getMetaData();

  std::list<jdbc::SQLString> types = {"TABLE"};
  std::unique_ptr<jdbc::ResultSet> tables(meta->getTables("", "kv_meta", "%", types));
  ASSERT_TRUE(tables->next());
  EXPECT_EQ(tables->getString("TABLE_CAT"), "def");
  EXPECT_EQ(tables->getString("TABLE_SCHEM"), "kv_meta");
  EXPECT_EQ(tables->getString("TABLE_NAME"), "t");
  EXPECT_EQ(tables->getString("TABLE_TYPE"), "TABLE");
  EXPECT_FALSE(tables->next());

  std::unique_ptr<jdbc::ResultSet> columns(meta->getColumns("", "kv_meta", "t", "%"));
  ASSERT_TRUE(columns->next());
  EXPECT_EQ(columns->getString("COLUMN_NAME"), "a");
  EXPECT_EQ(columns->getInt("DATA_TYPE"), jdbc::DataType::INTEGER);
  EXPECT_EQ(columns->getString("TYPE_NAME"), "INT");
  EXPECT_EQ(columns->getInt("COLUMN_SIZE"), 10);
  EXPECT_EQ(columns->getInt("NULLABLE"), jdbc::DatabaseMetaData::columnNoNulls);
  EXPECT_EQ(columns->getInt("ORDINAL_POSITION"), 1);
  EXPECT_EQ(columns->getString("IS_NULLABLE"), "NO");
  ASSERT_TRUE(columns->next());
  EXPECT_EQ(columns->getString("COLUMN_NAME"), "b");
  EXPECT_EQ(columns->getInt("DATA_TYPE"), jdbc::DataType::VARCHAR);
  EXPECT_EQ(columns->getString("TYPE_NAME"), "VARCHAR");
  EXPECT_EQ(columns->getInt("COLUMN_SIZE"), 10);
  EXPECT_EQ(columns->getInt("NULLABLE"), jdbc::DatabaseMetaData::columnNullable);
  EXPECT_EQ(columns->getInt("ORDINAL_POSITION"), 2);
  EXPECT_EQ(columns->getString("IS_NULLABLE"), "YES");
  EXPECT_FALSE(columns->next());
}

// A negative LIMIT bound through COM_STMT_EXECUTE: 1690, as MySQL 8.4.13
// answers it (observed through mysql-connector-python).
TEST_F(MysqlClient, ANegativeLimitParameterIsOutOfRange) {
  auto c = connect();
  std::unique_ptr<jdbc::PreparedStatement> select(c->prepareStatement("SELECT 1 LIMIT ?"));
  select->setInt(1, -1);
  const auto [code, state, message] = refusal([&] { std::unique_ptr<jdbc::ResultSet>(select->executeQuery()); });
  EXPECT_EQ(code, 1690) << message;
  EXPECT_EQ(state, "22003");
  EXPECT_EQ(message, "signed integer value is out of range in 'mysqld_stmt_execute'");
  select->setInt(1, 1);
  std::unique_ptr<jdbc::ResultSet> rows(select->executeQuery());
  EXPECT_TRUE(rows->next());
}

// A transaction rolled back leaves nothing behind (manual, "START
// TRANSACTION, COMMIT, and ROLLBACK Statements").
TEST_F(MysqlClient, ARolledBackTransactionLeavesNothing) {
  auto c = connect();
  std::unique_ptr<jdbc::Statement> statement(c->createStatement());
  statement->execute("CREATE DATABASE kv_txn");
  c->setSchema("kv_txn");
  statement->execute("CREATE TABLE t (a INT PRIMARY KEY)");
  c->setAutoCommit(false);
  statement->execute("INSERT INTO t VALUES (1)");
  c->rollback();
  c->setAutoCommit(true);
  EXPECT_EQ(single_int(*c, "SELECT COUNT(*) FROM t"), 0);
}

// ER_NO_SUCH_TABLE, 1146, SQLSTATE 42S02; the connection stays usable.
TEST_F(MysqlClient, AnUnknownTableIsAnErrorAndTheConnectionGoesOn) {
  auto c = connect();
  std::unique_ptr<jdbc::Statement> statement(c->createStatement());
  statement->execute("CREATE DATABASE kv_err");
  c->setSchema("kv_err");
  const auto [code, state, message] = refusal([&] { std::unique_ptr<jdbc::ResultSet>(statement->executeQuery("SELECT * FROM missing")); });
  EXPECT_EQ(code, 1146) << message;
  EXPECT_EQ(state, "42S02");
  EXPECT_EQ(message, "Table 'kv_err.missing' doesn't exist");
  EXPECT_EQ(single_int(*c, "SELECT 7"), 7);
}

}  // namespace
}  // namespace kvasir::server
