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

#include "dannan/contract/time_source.h"
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
struct SteppedTime : dannan::contract::TimeSource {
  dannan::contract::TimePoint at = dannan::contract::TimePoint{} + std::chrono::hours(100);
  dannan::contract::TimePoint now() const noexcept override { return at; }
};

std::string line(uint64_t uptime, uint64_t threads, uint64_t questions, const std::string& average) {
  return "Uptime: " + std::to_string(uptime) + "  Threads: " + std::to_string(threads) + "  Questions: " + std::to_string(questions) +
         "  Slow queries: 0  Opens: 0  Flush tables: 0  Open tables: 0  Queries per second avg: " + average;
}

TEST(ServerStatistics, TheLineMySqlSends) {
  SteppedTime time;
  NodeStatistics stats(time);
  EXPECT_EQ(stats.text(), line(0, 0, 0, "0.000"));
  time.at += std::chrono::milliseconds(540'900);
  for (int i = 0; i < 2115; ++i) stats.question();
  stats.command_began();
  stats.command_began();
  EXPECT_EQ(stats.text(), line(540, 2, 2115, "3.916"));
  stats.command_ended();
  time.at += std::chrono::seconds(2);
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
    time.at += std::chrono::seconds(10);
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

}  // namespace
}  // namespace kvasir::server
