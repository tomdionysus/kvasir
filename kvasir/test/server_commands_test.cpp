//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual, client/server protocol, "Command Phase": COM_QUIT,
// COM_INIT_DB, COM_QUERY, COM_PING, COM_RESET_CONNECTION; a command's packets
// start at sequence 0 and the reply at 1; a text result set is the column
// count, a ColumnDefinition41 per column, EOF unless CLIENT_DEPRECATE_EOF,
// the rows (each value length-encoded, 0xfb for NULL), then EOF, or OK with
// the 0xfe header under CLIENT_DEPRECATE_EOF; ER_UNKNOWN_COM_ERROR 1047
// (08S01) "Unknown command"; ER_PARSE_ERROR 1064 and ER_EMPTY_QUERY 1065
// (42000).

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "kvasir/protocol/messages.h"
#include "kvasir/server/commands.h"
#include "kvasir/server/statistics.h"

namespace kvasir::server {
namespace {

using protocol::Bytes;
using protocol::Packet;
using protocol::Reader;
namespace capability = protocol::capability;

const exec::VariableRegistry& registry() {
  static const exec::VariableRegistry r("replicated", "8.4.13-kvasir", "Kvasir - The Distributed Relational Database (in memory, not durable)");
  return r;
}

std::vector<Packet> packets(const Bytes& bytes) {
  protocol::PacketReader reader;
  reader.feed(bytes);
  std::vector<Packet> out;
  while (auto p = reader.next()) out.push_back(std::move(*p));
  EXPECT_EQ(reader.buffered(), 0u);
  return out;
}

Packet command(uint8_t code, std::string_view body = {}, uint8_t sequence = 0) {
  Packet p{sequence, sequence, {}};
  p.payload.push_back(code);
  p.payload.insert(p.payload.end(), body.begin(), body.end());
  return p;
}

struct Fixture {
  catalog::Catalog node;
  txn::Store rows;
  exec::Session session{registry(), node, rows, exec::SessionInfo{"root", "10.0.0.1", 7}};
  CommandPhase phase;
  explicit Fixture(uint32_t capabilities = capability::protocol_41 | capability::deprecate_eof) : phase(session, capabilities) {}
  std::vector<Packet> query(std::string_view sql) { return packets(phase.received(command(command::query, sql)).out); }
};

struct ColumnFields {
  std::string catalog, name;
  uint16_t charset = 0;
  uint32_t length = 0;
  uint8_t type = 0;
  uint16_t flags = 0;
  uint8_t decimals = 0;
};

ColumnFields column(const Packet& p) {
  Reader r(p.payload);
  ColumnFields c;
  c.catalog = r.lenenc_string(64);
  r.lenenc_string(64);
  r.lenenc_string(64);
  r.lenenc_string(64);
  c.name = r.lenenc_string(256);
  r.lenenc_string(256);
  EXPECT_EQ(r.lenenc(), 0x0cu);
  c.charset = r.u16();
  c.length = r.u32();
  c.type = r.u8();
  c.flags = r.u16();
  c.decimals = r.u8();
  return c;
}

struct OkFields {
  uint8_t header = 0;
  uint64_t affected = 0;
  uint16_t status = 0;
  uint16_t warnings = 0;
};

OkFields ok(const Packet& p) {
  Reader r(p.payload);
  OkFields o;
  o.header = r.u8();
  o.affected = r.lenenc();
  r.lenenc();
  o.status = r.u16();
  o.warnings = r.u16();
  return o;
}

struct ErrFields {
  uint16_t code = 0;
  std::string sqlstate, message;
};

ErrFields err(const Packet& p) {
  Reader r(p.payload);
  EXPECT_EQ(r.u8(), 0xff);
  ErrFields e;
  e.code = r.u16();
  EXPECT_EQ(r.u8(), '#');
  const auto state = r.view(5);
  e.sqlstate.assign(state.begin(), state.end());
  e.message = r.rest_string(4096);
  return e;
}

// Two clients of one node, for lock waits.
struct TwoClients {
  catalog::Catalog node;
  txn::Store rows;
  exec::Session a{registry(), node, rows, exec::SessionInfo{"root", "10.0.0.1", 7}};
  exec::Session b{registry(), node, rows, exec::SessionInfo{"root", "10.0.0.2", 8}};
  CommandPhase pa{a, capability::protocol_41 | capability::deprecate_eof};
  CommandPhase pb{b, capability::protocol_41 | capability::deprecate_eof};
  static CommandStep send(CommandPhase& p, std::string_view sql) { return p.received(command(command::query, sql)); }
  static void ran(CommandPhase& p, std::string_view sql) {
    const auto sent = packets(send(p, sql).out);
    ASSERT_EQ(sent.size(), 1u) << sql;
    EXPECT_EQ(sent[0].payload.at(0), 0x00) << sql;
  }
};

// InnoDB's lock waits (the manual, "InnoDB Locking" and "Deadlocks in
// InnoDB"): a writer that meets another transaction's lock waits for it to
// end, then goes on as if it had not waited; a wait past
// innodb_lock_wait_timeout ends the statement with 1205; a wait that closes a
// cycle ends the whole transaction with 1213.
TEST(ServerCommands, AWriterWaitsForAnothersLock) {
  TwoClients c;
  EXPECT_EQ(c.b.lock_wait_timeout(), 50u);
  TwoClients::ran(c.pa, "CREATE DATABASE d");
  TwoClients::ran(c.pa, "USE d");
  TwoClients::ran(c.pb, "USE d");
  TwoClients::ran(c.pa, "CREATE TABLE t (a INT PRIMARY KEY)");
  TwoClients::ran(c.pa, "BEGIN");
  TwoClients::ran(c.pa, "INSERT INTO t VALUES (7)");
  auto step = TwoClients::send(c.pb, "INSERT INTO t VALUES (7)");
  EXPECT_TRUE(step.waiting);
  EXPECT_TRUE(step.out.empty());
  int woken = 0;
  step = c.pb.wait([&] { ++woken; });
  EXPECT_TRUE(step.waiting);
  EXPECT_TRUE(step.out.empty());
  TwoClients::ran(c.pa, "COMMIT");
  EXPECT_EQ(woken, 1);
  step = c.pb.resume();
  EXPECT_FALSE(step.waiting);
  auto sent = packets(step.out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 1u);
  EXPECT_EQ(err(sent[0]).code, 1062);

  // Too long a wait: the statement ends, the transaction does not.
  TwoClients::ran(c.pa, "BEGIN");
  TwoClients::ran(c.pa, "INSERT INTO t VALUES (8)");
  TwoClients::ran(c.pb, "BEGIN");
  TwoClients::ran(c.pb, "INSERT INTO t VALUES (9)");
  EXPECT_TRUE(TwoClients::send(c.pb, "INSERT INTO t VALUES (8)").waiting);
  EXPECT_TRUE(c.pb.wait([&] { ++woken; }).waiting);
  sent = packets(c.pb.give_up().out);
  ASSERT_EQ(sent.size(), 1u);
  const auto timeout = err(sent[0]);
  EXPECT_EQ(timeout.code, 1205);
  EXPECT_EQ(timeout.sqlstate, "HY000");
  EXPECT_EQ(timeout.message, "Lock wait timeout exceeded; try restarting transaction");
  EXPECT_TRUE(c.b.in_transaction());
  TwoClients::ran(c.pa, "ROLLBACK");
  EXPECT_EQ(woken, 1);

  // Each waits for the other: the second to wait is the victim, rolled back
  // whole, and the first goes on.
  TwoClients::ran(c.pa, "BEGIN");
  TwoClients::ran(c.pa, "INSERT INTO t VALUES (10)");
  EXPECT_TRUE(TwoClients::send(c.pa, "INSERT INTO t VALUES (9)").waiting);
  int a_woken = 0;
  EXPECT_TRUE(c.pa.wait([&] { ++a_woken; }).waiting);
  EXPECT_TRUE(TwoClients::send(c.pb, "INSERT INTO t VALUES (10)").waiting);
  sent = packets(c.pb.wait([] {}).out);
  ASSERT_EQ(sent.size(), 1u);
  const auto deadlock = err(sent[0]);
  EXPECT_EQ(deadlock.code, 1213);
  EXPECT_EQ(deadlock.sqlstate, "40001");
  EXPECT_EQ(deadlock.message, "Deadlock found when trying to get lock; try restarting transaction");
  EXPECT_FALSE(c.b.in_transaction());
  EXPECT_EQ(a_woken, 1);
  sent = packets(c.pa.resume().out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(ok(sent[0]).affected, 1u);
  TwoClients::ran(c.pa, "COMMIT");
  // b's row 9 went with its transaction; a's 9 and 10 are there.
  sent = packets(TwoClients::send(c.pb, "SELECT a FROM t ORDER BY a").out);
  EXPECT_EQ(sent.size(), 6u);
}

TEST(ServerCommands, TheVersionCommentQueryWithDeprecateEof) {
  Fixture f;
  const auto sent = f.query("select @@version_comment limit 1");
  ASSERT_EQ(sent.size(), 4u);
  for (size_t i = 0; i < sent.size(); ++i) EXPECT_EQ(sent[i].first_sequence, i + 1);
  EXPECT_EQ(sent[0].payload, (Bytes{0x01}));
  const auto c = column(sent[1]);
  EXPECT_EQ(c.catalog, "def");
  EXPECT_EQ(c.name, "@@version_comment");
  EXPECT_EQ(c.charset, 255);
  EXPECT_EQ(c.length, 87380u);
  EXPECT_EQ(c.type, 253);
  EXPECT_EQ(c.flags, 0);
  EXPECT_EQ(c.decimals, 31);
  Reader row(sent[2].payload);
  EXPECT_EQ(row.lenenc_string(256), "Kvasir - The Distributed Relational Database (in memory, not durable)");
  const auto end = ok(sent[3]);
  EXPECT_EQ(end.header, 0xfe);
  EXPECT_EQ(end.status, protocol::status::autocommit);
  EXPECT_EQ(end.warnings, 0);
}

TEST(ServerCommands, AResultSetWithEofPackets) {
  Fixture f(capability::protocol_41);
  const auto sent = f.query("SELECT 1, NULL");
  ASSERT_EQ(sent.size(), 6u);
  EXPECT_EQ(sent[0].payload, (Bytes{0x02}));
  EXPECT_EQ(column(sent[1]).name, "1");
  EXPECT_EQ(column(sent[2]).name, "NULL");
  // EOF after the definitions: 0xfe, warnings, status.
  EXPECT_EQ(sent[3].payload, (Bytes{0xfe, 0x00, 0x00, 0x02, 0x00}));
  EXPECT_EQ(sent[4].payload, (Bytes{0x01, '1', 0xfb}));
  EXPECT_EQ(sent[5].payload, (Bytes{0xfe, 0x00, 0x00, 0x02, 0x00}));
  EXPECT_EQ(sent[5].first_sequence, 6);
}

TEST(ServerCommands, NoRowsStillEndsTheResultSet) {
  Fixture f;
  const auto sent = f.query("SELECT 1 LIMIT 0");
  ASSERT_EQ(sent.size(), 3u);
  EXPECT_EQ(ok(sent[2]).header, 0xfe);
}

TEST(ServerCommands, AStatementWithoutRowsIsOk) {
  Fixture f;
  auto sent = f.query("SET autocommit = 0");
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 1);
  const auto o = ok(sent[0]);
  EXPECT_EQ(o.header, 0x00);
  EXPECT_EQ(o.affected, 0u);
  EXPECT_EQ(o.status, 0);
  sent = f.query("BEGIN");
  EXPECT_EQ(ok(sent[0]).status, protocol::status::in_transaction);
}

TEST(ServerCommands, WarningsAreCounted) {
  Fixture f;
  auto sent = f.query("SELECT 1/0");
  EXPECT_EQ(ok(sent.back()).warnings, 1);
  sent = f.query("SET wait_timeout = 0");
  EXPECT_EQ(ok(sent[0]).warnings, 1);
}

TEST(ServerCommands, StatementErrorsAreErr) {
  Fixture f;
  auto sent = f.query("SELEC 1");
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 1);
  auto e = err(sent[0]);
  EXPECT_EQ(e.code, 1064);
  EXPECT_EQ(e.sqlstate, "42000");
  EXPECT_EQ(
      e.message,
      "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near 'SELEC 1' at "
      "line 1");
  e = err(f.query("  ")[0]);
  EXPECT_EQ(e.code, 1065);
  EXPECT_EQ(e.sqlstate, "42000");
  EXPECT_EQ(e.message, "Query was empty");
  e = err(f.query("SELECT 9223372036854775807 + 1")[0]);
  EXPECT_EQ(e.code, 1690);
  EXPECT_EQ(e.sqlstate, "22003");
  e = err(f.query("SET nope = 1")[0]);
  EXPECT_EQ(e.code, 1193);
}

TEST(ServerCommands, OneStatementAtATime) {
  Fixture f;
  const auto e = err(f.query("SELECT 1; SELECT 2")[0]);
  EXPECT_EQ(e.code, 1064);
  EXPECT_NE(e.message.find("near 'SELECT 2'"), std::string::npos) << e.message;
  // A trailing semicolon is one statement.
  EXPECT_EQ(f.query("SELECT 1;").size(), 4u);
}

TEST(ServerCommands, PingAndQuit) {
  Fixture f;
  auto step = f.phase.received(command(command::ping));
  auto sent = packets(step.out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 1);
  EXPECT_EQ(ok(sent[0]).header, 0x00);
  EXPECT_FALSE(step.close);
  step = f.phase.received(command(command::quit));
  EXPECT_TRUE(step.out.empty());
  EXPECT_TRUE(step.close);
}

TEST(ServerCommands, InitDbKnowsNoDatabaseYet) {
  Fixture f;
  const auto e = err(packets(f.phase.received(command(command::init_db, "shop")).out)[0]);
  EXPECT_EQ(e.code, 1049);
  EXPECT_EQ(e.message, "Unknown database 'shop'");
}

TEST(ServerCommands, ResetConnectionForgetsTheSession) {
  Fixture f;
  f.query("SET @a = 1, autocommit = 0");
  const auto sent = packets(f.phase.received(command(command::reset_connection)).out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(ok(sent[0]).status, protocol::status::autocommit);
  const auto rows = f.query("SELECT @a, @@autocommit");
  Reader row(rows[3].payload);
  EXPECT_EQ(row.u8(), 0xfb);
  EXPECT_EQ(row.lenenc_string(8), "1");
}

TEST(ServerCommands, UnknownCommandsAreRefused) {
  Fixture f;
  auto sent = packets(f.phase.received(command(0x99)).out);
  ASSERT_EQ(sent.size(), 1u);
  const auto e = err(sent[0]);
  EXPECT_EQ(e.code, 1047);
  EXPECT_EQ(e.sqlstate, "08S01");
  EXPECT_EQ(e.message, "Unknown command");
  EXPECT_EQ(err(packets(f.phase.received(Packet{0, 0, {}}).out)[0]).code, 1047);
}

TEST(ServerCommands, ACommandOutOfSequenceIsRefusedAndCloses) {
  Fixture f;
  const auto step = f.phase.received(command(command::ping, {}, 3));
  EXPECT_TRUE(step.close);
  const auto e = err(packets(step.out)[0]);
  EXPECT_EQ(e.code, 1156);
  EXPECT_EQ(e.sqlstate, "08S01");
}

// MySQL's server sends no NUM flag (0x8000); its client library adds it.
// Captured from MySQL 8.4.13 (Go's driver, 2026-11-20): SELECT a, 1 + 1, 2.5,
// 1e0 FROM t sent 1 + 1 as LONGLONG 3 flags 0x0081, 2.5 NEWDECIMAL 4 0x0081
// with 1 decimal, 1e0 DOUBLE 3 0x0081 with 31.
TEST(ServerCommands, NoNumFlagOnTheWire) {
  Fixture f;
  const auto sent = f.query("SELECT 1 + 1, 2.5, 1e0");
  ASSERT_GE(sent.size(), 4u);
  const auto a = column(sent[1]), b = column(sent[2]), c = column(sent[3]);
  EXPECT_EQ(a.type, 8);
  EXPECT_EQ(a.length, 3u);
  EXPECT_EQ(a.flags, 0x0081);
  EXPECT_EQ(b.type, 246);
  EXPECT_EQ(b.length, 4u);
  EXPECT_EQ(b.flags, 0x0081);
  EXPECT_EQ(b.decimals, 1);
  EXPECT_EQ(c.type, 5);
  EXPECT_EQ(c.flags, 0x0081);
  EXPECT_EQ(c.decimals, 31);
}

// Prepared statements: the bytes MySQL 8.4.13 exchanged with Go's driver
// (go-sql-driver/mysql 1.8.1, no CLIENT_DEPRECATE_EOF) and
// mysql-connector-python (with it), captured through a proxy on 2026-11-20,
// and the manual's COM_STMT_PREPARE, COM_STMT_EXECUTE,
// COM_STMT_SEND_LONG_DATA, COM_STMT_CLOSE and COM_STMT_RESET.
Bytes hex(std::string_view text) {
  Bytes out;
  std::string digits;
  for (const char ch : text)
    if (ch != ' ') digits.push_back(ch);
  for (size_t i = 0; i + 1 < digits.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(digits.substr(i, 2), nullptr, 16)));
  return out;
}

struct PreparedBed {
  catalog::Catalog node;
  txn::Store rows;
  exec::Session session{registry(), node, rows, exec::SessionInfo{"root", "10.0.0.1", 7}};
  CommandPhase phase;
  explicit PreparedBed(uint32_t capabilities = capability::protocol_41) : phase(session, capabilities) {
    for (const char* sql :
         {"CREATE DATABASE kv_prep", "USE kv_prep", "CREATE TABLE t (a INT PRIMARY KEY, b VARCHAR(10))", "INSERT INTO t VALUES (1, 'x'), (2, 'y')"})
      phase.received(command(command::query, sql));
  }
  std::vector<Packet> send(uint8_t code, const Bytes& body) {
    return packets(phase.received(command(code, std::string_view(reinterpret_cast<const char*>(body.data()), body.size()))).out);
  }
  std::vector<Packet> prepare(std::string_view sql) { return packets(phase.received(command(command::stmt_prepare, sql)).out); }
};

const Bytes eof = hex("fe 0000 0200");

TEST(ServerCommands, PrepareAndExecuteAsGosDriverDoes) {
  PreparedBed b;
  auto sent = b.prepare("SELECT a, b FROM t WHERE a > ? AND b <> ?");
  ASSERT_EQ(sent.size(), 7u);
  for (size_t i = 0; i < sent.size(); ++i) EXPECT_EQ(sent[i].first_sequence, i + 1);
  EXPECT_EQ(sent[0].payload, hex("00 01000000 0200 0200 00 0000"));
  EXPECT_EQ(sent[1].payload, hex("03646566 00 00 00 013f 00 0c 3f00 15000000 08 8000 00 0000"));
  EXPECT_EQ(sent[2].payload, hex("03646566 00 00 00 013f 00 0c ff00 fcff0000 fd 0000 1f 0000"));
  EXPECT_EQ(sent[3].payload, eof);
  EXPECT_EQ(sent[4].payload, hex("03646566 076b765f70726570 0174 0174 0161 0161 0c 3f00 0b000000 03 0350 00 0000"));
  EXPECT_EQ(sent[5].payload, hex("03646566 076b765f70726570 0174 0174 0162 0162 0c ff00 28000000 fd 0000 00 0000"));
  EXPECT_EQ(sent[6].payload, eof);

  sent = b.send(command::stmt_execute, hex("01000000 00 01000000 00 01 0800 fe00 0000000000000000 027a7a"));
  ASSERT_EQ(sent.size(), 7u);
  for (size_t i = 0; i < sent.size(); ++i) EXPECT_EQ(sent[i].first_sequence, i + 1);
  EXPECT_EQ(sent[0].payload, hex("02"));
  EXPECT_EQ(sent[1].payload, hex("03646566 076b765f70726570 0174 0174 0161 0161 0c 3f00 0b000000 03 0350 00 0000"));
  EXPECT_EQ(sent[3].payload, eof);
  EXPECT_EQ(sent[4].payload, hex("00 00 01000000 0178"));
  EXPECT_EQ(sent[5].payload, hex("00 00 02000000 0179"));
  EXPECT_EQ(sent[6].payload, eof);

  // Again, with the types bound before.
  sent = b.send(command::stmt_execute, hex("01000000 00 01000000 00 00 0100000000000000 0178"));
  ASSERT_EQ(sent.size(), 6u);
  EXPECT_EQ(sent[4].payload, hex("00 00 02000000 0179"));

  // COM_STMT_CLOSE has no reply; the statement is then unknown.
  EXPECT_TRUE(b.send(command::stmt_close, hex("01000000")).empty());
  sent = b.send(command::stmt_execute, hex("01000000 00 01000000 00 01 0800 fe00 0000000000000000 027a7a"));
  ASSERT_EQ(sent.size(), 1u);
  const auto e = err(sent[0]);
  EXPECT_EQ(e.code, 1243);
  EXPECT_EQ(e.message, "Unknown prepared statement handler (1) given to mysqld_stmt_execute");
}

TEST(ServerCommands, ExecutedValuesAndChanges) {
  PreparedBed b;
  // SELECT ?, ? + 1, CONCAT(?, 'x'), ? with -5, 2.5, "ab", NULL.
  auto sent = b.prepare("SELECT ?, ? + 1, CONCAT(?, 'x'), ?");
  ASSERT_EQ(sent.size(), 11u);
  EXPECT_EQ(sent[0].payload, hex("00 01000000 0400 0400 00 0000"));
  sent = b.send(command::stmt_execute, hex("01000000 00 01000000 08 01 0800 0500 fe00 0600 fbffffffffffffff 0000000000000440 026162"));
  ASSERT_EQ(sent.size(), 8u);
  EXPECT_EQ(sent[1].payload, hex("03646566 00 00 00 013f 00 0c 3f00 15000000 08 8000 00 0000"));
  EXPECT_EQ(sent[2].payload, hex("03646566 00 00 00 05 3f202b2031 00 0c 3f00 17000000 05 8000 1f 0000"));
  EXPECT_EQ(sent[3].payload, hex("03646566 00 00 00 0e 434f4e434154283f2c2027782729 00 0c ff00 00000400 fa 0000 1f 0000"));
  EXPECT_EQ(sent[6].payload, hex("00 20 fbffffffffffffff 0000000000000c40 03616278"));

  // INSERT and DELETE answer OK.
  sent = b.prepare("INSERT INTO t VALUES (?, ?)");
  EXPECT_EQ(sent[0].payload, hex("00 02000000 0000 0200 00 0000"));
  ASSERT_EQ(sent.size(), 4u);
  sent = b.send(command::stmt_execute, hex("02000000 00 01000000 02 01 0800 0600 0900000000000000"));
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload, hex("00 01 00 0200 0000"));
  sent = b.prepare("DELETE FROM t WHERE a = ?");
  sent = b.send(command::stmt_execute, hex("03000000 00 01000000 00 01 0800 0900000000000000"));
  EXPECT_EQ(sent[0].payload, hex("00 01 00 0200 0000"));
}

TEST(ServerCommands, PreparedWithDeprecateEof) {
  // mysql-connector-python: no EOF after the definitions, the rows ended by
  // an OK with the EOF header; COM_STMT_RESET answered OK.
  PreparedBed b(capability::protocol_41 | capability::deprecate_eof);
  auto sent = b.prepare("SELECT ?, ?, ?, ?");
  ASSERT_EQ(sent.size(), 9u);
  sent = b.send(command::stmt_reset, hex("01000000"));
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload, hex("00 00 00 0200 0000"));
  sent = b.send(command::stmt_execute, hex("01000000 00 01000000 08 01 0000 fe00 0800 0600 04312e3235 027879 0700000000000000"));
  ASSERT_EQ(sent.size(), 7u);
  EXPECT_EQ(sent[1].payload, hex("03646566 00 00 00 013f 00 0c 3f00 43000000 f6 8000 1e 0000"));
  EXPECT_EQ(sent[5].payload, hex("00 20 04312e3235 027879 0700000000000000"));
  EXPECT_EQ(sent[6].payload, hex("fe 00 00 0200 0000"));
  // An unknown statement to reset.
  EXPECT_EQ(err(b.send(command::stmt_reset, hex("09000000"))[0]).message, "Unknown prepared statement handler (9) given to mysqld_stmt_reset");
}

TEST(ServerCommands, LongDataAndWhatIsRefused) {
  PreparedBed b;
  b.prepare("SELECT a FROM t WHERE b = ? AND a > ?");
  // COM_STMT_SEND_LONG_DATA: statement, parameter, bytes; no reply. The
  // execute then carries no value for that parameter.
  EXPECT_TRUE(b.send(command::stmt_send_long_data, hex("01000000 0000 78")).empty());
  auto sent = b.send(command::stmt_execute, hex("01000000 00 01000000 00 01 fe00 0800 0000000000000000"));
  ASSERT_EQ(sent.size(), 5u);
  EXPECT_EQ(sent[3].payload, hex("00 00 01000000"));
  // COM_STMT_RESET forgets it.
  b.send(command::stmt_send_long_data, hex("01000000 0000 79"));
  b.send(command::stmt_reset, hex("01000000"));
  sent = b.send(command::stmt_execute, hex("01000000 00 01000000 00 01 fe00 0800 0179 0000000000000000"));
  EXPECT_EQ(sent[3].payload, hex("00 00 02000000"));

  // Bytes that do not fit.
  auto e = err(b.send(command::stmt_execute, hex("01000000 00 01000000 00 01 fe00"))[0]);
  EXPECT_EQ(e.code, 1835);
  EXPECT_EQ(e.message, "Malformed communication packet.");
  // A cursor.
  e = err(b.send(command::stmt_execute, hex("01000000 01 01000000 00 01 fe00 0800 0178 0000000000000000"))[0]);
  EXPECT_EQ(e.code, 1235);
  // A syntax error at the prepare.
  e = err(b.prepare("SELECT FROM")[0]);
  EXPECT_EQ(e.code, 1064);
  // Two statements in one prepare.
  EXPECT_EQ(err(b.prepare("SELECT 1; SELECT 2")[0]).code, 1064);
}

// COM_STATISTICS (the manual, "COM_STATISTICS"): the reply is one
// human-readable string, with no header byte. Observed on MySQL 8.4.13
// (2026-11-19), the `mysql` client's `status` command sends it, and MySQL
// answers e.g. "Uptime: 540  Threads: 2  Questions: 2115  Slow queries: 0
// Opens: 383  Flush tables: 3  Open tables: 203  Queries per second avg:
// 3.916": two spaces between fields; the average Questions / Uptime cut,
// not rounded, to three places (2115 / 540 = 3.9166); Threads those running
// a command, the asker included. Questions counts a sign-in and every
// command but COM_PING and COM_STATISTICS (the manual, "Questions"; measured:
// a connection that only pings counts 2, its sign-in and its COM_QUIT; a
// statement refused counts; a failed sign-in counts nothing). Kvasir has no
// table cache and no slow query log: those fields are 0.
using dannan::test::SteppedTime;

std::string line(uint64_t uptime, uint64_t threads, uint64_t questions, const std::string& average) {
  return "Uptime: " + std::to_string(uptime) + "  Threads: " + std::to_string(threads) + "  Questions: " + std::to_string(questions) +
         "  Slow queries: 0  Opens: 0  Flush tables: 0  Open tables: 0  Queries per second avg: " + average;
}

TEST(ServerStatistics, TheLineMySqlSends) {
  SteppedTime time;
  NodeStatistics stats(time);
  EXPECT_EQ(stats.text(), line(0, 0, 0, "0.000"));
  time.advance(std::chrono::milliseconds(540'900));
  for (int i = 0; i < 2115; ++i) stats.question();
  stats.command_began();
  stats.command_began();
  EXPECT_EQ(stats.text(), line(540, 2, 2115, "3.916"));
  stats.command_ended();
  time.advance(std::chrono::seconds(2));
  stats.question();
  stats.question();
  // 2117 / 542 = 3.9059
  EXPECT_EQ(stats.text(), line(542, 1, 2117, "3.905"));
}

TEST(ServerCommands, StatisticsAnswersWithOneString) {
  SteppedTime time;
  NodeStatistics stats(time);
  catalog::Catalog node;
  txn::Store rows;
  exec::Session session{registry(), node, rows, exec::SessionInfo{"root", "10.0.0.1", 7}};
  {
    // The sign-in counts.
    CommandPhase phase(session, capability::protocol_41 | capability::deprecate_eof, &stats);
    EXPECT_EQ(stats.text(), line(0, 0, 1, "0.000"));
    phase.received(command(command::query, "SELECT 1"));
    phase.received(command(command::query, "SELECT nope"));
    phase.received(command(command::ping));
    time.advance(std::chrono::seconds(10));
    auto sent = packets(phase.received(command(command::statistics)).out);
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].first_sequence, 1u);
    EXPECT_EQ(std::string(sent[0].payload.begin(), sent[0].payload.end()), line(10, 1, 3, "0.300"));
    // Nothing runs between commands; COM_STATISTICS is not counted.
    EXPECT_EQ(stats.text(), line(10, 0, 3, "0.300"));
    phase.received(command(command::init_db, "nope"));
    EXPECT_EQ(stats.text(), line(10, 0, 4, "0.400"));
    phase.received(command(command::quit));
    EXPECT_EQ(stats.text(), line(10, 0, 5, "0.500"));
  }
  // A phase let go of while no command runs leaves the count as it was.
  EXPECT_EQ(stats.text(), line(10, 0, 5, "0.500"));
}

TEST(ServerCommands, AWaitingStatementIsRunning) {
  SteppedTime time;
  NodeStatistics stats(time);
  catalog::Catalog node;
  txn::Store rows;
  exec::Session a{registry(), node, rows, exec::SessionInfo{"root", "10.0.0.1", 7}};
  exec::Session b{registry(), node, rows, exec::SessionInfo{"root", "10.0.0.2", 8}};
  CommandPhase pa{a, capability::protocol_41 | capability::deprecate_eof, &stats};
  {
    CommandPhase pb{b, capability::protocol_41 | capability::deprecate_eof, &stats};
    for (const char* sql : {"CREATE DATABASE d", "USE d", "CREATE TABLE t (a INT PRIMARY KEY)", "BEGIN", "INSERT INTO t VALUES (7)"}) TwoClients::ran(pa, sql);
    TwoClients::ran(pb, "USE d");
    EXPECT_EQ(stats.text(), line(0, 0, 8, "0.000"));
    EXPECT_TRUE(TwoClients::send(pb, "INSERT INTO t VALUES (7)").waiting);
    EXPECT_TRUE(pb.wait([] {}).waiting);
    // Counted once, and running while it waits.
    EXPECT_EQ(stats.text(), line(0, 1, 9, "0.000"));
    TwoClients::ran(pa, "COMMIT");
    EXPECT_FALSE(pb.resume().waiting);
    EXPECT_EQ(stats.text(), line(0, 0, 10, "0.000"));
    // Given up: no longer running.
    TwoClients::ran(pa, "BEGIN");
    TwoClients::ran(pa, "INSERT INTO t VALUES (8)");
    EXPECT_TRUE(TwoClients::send(pb, "INSERT INTO t VALUES (8)").waiting);
    EXPECT_EQ(stats.text(), line(0, 1, 13, "0.000"));
    pb.give_up();
    EXPECT_EQ(stats.text(), line(0, 0, 13, "0.000"));
    // Let go of while it waits (the client left): no longer running.
    EXPECT_TRUE(TwoClients::send(pb, "INSERT INTO t VALUES (8)").waiting);
    EXPECT_EQ(stats.text(), line(0, 1, 14, "0.000"));
  }
  EXPECT_EQ(stats.text(), line(0, 0, 14, "0.000"));
}

TEST(ServerCommands, PreparedStatementsAndQuestions) {
  // The manual's "Questions" counts neither COM_STMT_PREPARE, COM_STMT_CLOSE
  // nor COM_STMT_RESET; COM_STMT_EXECUTE counts.
  SteppedTime time;
  NodeStatistics stats(time);
  catalog::Catalog node;
  txn::Store rows;
  exec::Session session{registry(), node, rows, exec::SessionInfo{"root", "10.0.0.1", 7}};
  CommandPhase phase(session, capability::protocol_41, &stats);
  phase.received(command(command::stmt_prepare, "SELECT ?"));
  const Bytes reset = hex("01000000");
  phase.received(command(command::stmt_reset, std::string_view(reinterpret_cast<const char*>(reset.data()), reset.size())));
  const Bytes execute = hex("01000000 00 01000000 00 01 0800 0100000000000000");
  phase.received(command(command::stmt_execute, std::string_view(reinterpret_cast<const char*>(execute.data()), execute.size())));
  phase.received(command(command::stmt_close, std::string_view(reinterpret_cast<const char*>(reset.data()), reset.size())));
  EXPECT_EQ(stats.text(), line(0, 0, 2, "0.000"));
}

// COM_INIT_DB of a database that exists answers OK and makes it current
// (the manual, COM_INIT_DB).
TEST(ServerCommands, InitDbMakesADatabaseCurrent) {
  Fixture f;
  f.query("CREATE DATABASE shop");
  const auto sent = packets(f.phase.received(command(command::init_db, "shop")).out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload.at(0), 0x00);
  EXPECT_EQ(f.session.database(), std::optional<std::string>("shop"));
}

// What COM_STMT_PREPARE and COM_STMT_EXECUTE refuse, in MySQL's words: an
// empty query (1065), what the statement itself refuses (1146), more
// placeholders than the answer can count (1390); a statement refused as it
// runs; a parameter typed NULL is NULL whatever the bitmap says (the
// manual, COM_STMT_EXECUTE); a body too short to name a statement (1835).
TEST(ServerCommands, PreparedRefusals) {
  PreparedBed b;
  auto sent = b.prepare("INSERT INTO t VALUES (?, 'z')");
  ASSERT_EQ(sent[0].payload.at(0), 0x00);
  sent = b.send(command::stmt_execute, hex("01000000 00 01000000 00 01 0800 0100000000000000"));
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(err(sent[0]).code, 1062);
  EXPECT_EQ(err(sent[0]).message, "Duplicate entry '1' for key 't.PRIMARY'");

  sent = b.prepare("SELECT ?");
  ASSERT_EQ(sent[0].payload.at(0), 0x00);
  sent = b.send(command::stmt_execute, hex("02000000 00 01000000 00 01 0600"));
  ASSERT_EQ(sent.size(), 5u);
  EXPECT_EQ(sent[3].payload, hex("00 04"));

  sent = b.prepare("");
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(err(sent[0]).code, 1065);
  EXPECT_EQ(err(sent[0]).message, "Query was empty");
  sent = b.prepare("SELECT * FROM nope");
  EXPECT_EQ(err(sent[0]).code, 1146);
  EXPECT_EQ(err(sent[0]).message, "Table 'kv_prep.nope' doesn't exist");
  std::string many = "SELECT ?";
  for (int i = 0; i < 65535; ++i) many += ",?";
  sent = b.prepare(many);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(err(sent[0]).code, 1390);
  EXPECT_EQ(err(sent[0]).message, "Prepared statement contains too many placeholders");

  sent = b.send(command::stmt_close, hex("0100"));
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(err(sent[0]).code, 1835);
}

// A prepared statement that meets another transaction's lock waits as a
// query does, then answers as MySQL would have (here 1062).
TEST(ServerCommands, APreparedWriterWaitsForAnothersLock) {
  TwoClients c;
  TwoClients::ran(c.pa, "CREATE DATABASE d");
  TwoClients::ran(c.pa, "USE d");
  TwoClients::ran(c.pb, "USE d");
  TwoClients::ran(c.pa, "CREATE TABLE t (a INT PRIMARY KEY)");
  TwoClients::ran(c.pa, "BEGIN");
  TwoClients::ran(c.pa, "INSERT INTO t VALUES (7)");
  c.pb.received(command(command::stmt_prepare, "INSERT INTO t VALUES (?)"));
  const Bytes execute = hex("01000000 00 01000000 00 01 0800 0700000000000000");
  auto step = c.pb.received(command(command::stmt_execute, std::string_view(reinterpret_cast<const char*>(execute.data()), execute.size())));
  EXPECT_TRUE(step.waiting);
  EXPECT_TRUE(step.out.empty());
  EXPECT_TRUE(c.pb.wait([] {}).waiting);
  TwoClients::ran(c.pa, "COMMIT");
  const auto sent = packets(c.pb.resume().out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(err(sent[0]).code, 1062);
}

}  // namespace
}  // namespace kvasir::server
