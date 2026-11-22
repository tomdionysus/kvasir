//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A connection end to end over a fake transport: the greeting, SSLRequest,
// the TLS upgrade, the response, the full path with its password checked away
// from the connection, the command phase. MySQL 8.4 manual, "Connection
// Phase" and "Command Phase"; the exact-read contract is Dannan's for
// start_tls_unanswered (anything read past the SSLRequest is lost to the TLS
// handshake), and the design's (TLS before authentication).

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <deque>
#include <string>
#include <vector>

#include "kvasir/protocol/caching_sha2.h"
#include "kvasir/protocol/messages.h"
#include "kvasir/server/connection.h"

namespace kvasir::server {
namespace {

using protocol::Bytes;
using protocol::Packet;
using protocol::Reader;
using protocol::Writer;
namespace capability = protocol::capability;
namespace sha2 = protocol::caching_sha2;

const exec::VariableRegistry& registry() {
  static const exec::VariableRegistry r("ephemeral", "8.4.13-kvasir", "Kvasir - The Distributed Relational Database (in memory, not durable)");
  return r;
}

// Bytes in, bytes out, on an io_context: every completion posted.
class FakeTransport : public Transport {
 public:
  explicit FakeTransport(boost::asio::io_context& io) : _io(io) {}

  void read_some(std::span<uint8_t> buffer, std::function<void(std::optional<size_t>)> done) override {
    largest_read = std::max(largest_read, buffer.size());
    _pending = Read{buffer, std::move(done)};
    deliver();
  }
  void send(std::string bytes) override { sent.insert(sent.end(), bytes.begin(), bytes.end()); }
  void start_tls(std::function<void(std::string)> done) override {
    ++tls_started;
    unread_at_tls = inbound.size();
    boost::asio::post(_io, [done = std::move(done), this] { done(tls_fails ? "handshake failed" : ""); });
  }
  // As Dannan's stream does, a read pending when the stream closes ends.
  void close() override {
    closed = true;
    hang_up();
  }
  void abort() override {
    aborted = true;
    hang_up();
  }
  std::string peer_host() const override { return "10.0.0.1"; }
  bool tls_available() const override { return true; }

  void push(const Bytes& bytes) {
    inbound.insert(inbound.end(), bytes.begin(), bytes.end());
    deliver();
  }
  void hang_up() {
    hung_up = true;
    deliver();
  }

  std::deque<uint8_t> inbound;
  Bytes sent;
  size_t largest_read = 0;
  int tls_started = 0;
  size_t unread_at_tls = 0;
  bool tls_fails = false;
  bool closed = false;
  bool aborted = false;
  bool hung_up = false;

 private:
  struct Read {
    std::span<uint8_t> buffer;
    std::function<void(std::optional<size_t>)> done;
  };
  void deliver() {
    if (!_pending || (inbound.empty() && !hung_up)) return;
    auto read = std::move(*_pending);
    _pending.reset();
    if (inbound.empty()) {
      boost::asio::post(_io, [done = std::move(read.done)] { done(std::nullopt); });
      return;
    }
    const size_t n = std::min(read.buffer.size(), inbound.size());
    std::copy_n(inbound.begin(), n, read.buffer.begin());
    inbound.erase(inbound.begin(), inbound.begin() + static_cast<std::ptrdiff_t>(n));
    boost::asio::post(_io, [done = std::move(read.done), n] { done(n); });
  }

  boost::asio::io_context& _io;
  std::optional<Read> _pending;
};

// Holds each check until the test answers it.
class FakeChecker : public PasswordChecker {
 public:
  void check(std::string user, dannan::identity::Secret password, boost::asio::any_io_executor executor, std::function<void(bool)> done) override {
    asked.push_back({user, std::string(password.reveal().begin(), password.reveal().end())});
    _executor = executor;
    _done = std::move(done);
  }
  void answer(bool matches) {
    boost::asio::post(_executor, [done = std::move(_done), matches] { done(matches); });
  }
  std::vector<std::pair<std::string, std::string>> asked;

 private:
  boost::asio::any_io_executor _executor;
  std::function<void(bool)> _done;
};

Bytes framed(const Bytes& payload, uint8_t sequence) {
  Bytes out;
  protocol::frame(payload, sequence, out);
  return out;
}

inline constexpr uint32_t client_flags = capability::protocol_41 | capability::secure_connection | capability::plugin_auth |
                                         capability::plugin_auth_lenenc_client_data | capability::deprecate_eof | capability::ssl | capability::transactions;

Bytes ssl_request() {
  Writer w;
  w.u32(client_flags);
  w.u32(16 * 1024 * 1024);
  w.u8(255);
  w.zeros(23);
  return w.take();
}

Bytes response(const std::array<uint8_t, protocol::nonce_size>& nonce, const std::string& password) {
  const auto scramble = sha2::scramble({reinterpret_cast<const uint8_t*>(password.data()), password.size()}, nonce);
  Writer w;
  w.u32(client_flags);
  w.u32(16 * 1024 * 1024);
  w.u8(255);
  w.zeros(23);
  w.nul_string("root");
  w.lenenc(scramble.size());
  w.raw(scramble);
  w.nul_string(std::string(sha2::plugin_name));
  return w.take();
}

// The response with CLIENT_CONNECT_WITH_DB and a database.
Bytes response_with_database(const std::array<uint8_t, protocol::nonce_size>& nonce, const std::string& password, const std::string& database) {
  const auto scramble = sha2::scramble({reinterpret_cast<const uint8_t*>(password.data()), password.size()}, nonce);
  Writer w;
  w.u32(client_flags | capability::connect_with_db);
  w.u32(16 * 1024 * 1024);
  w.u8(255);
  w.zeros(23);
  w.nul_string("root");
  w.lenenc(scramble.size());
  w.raw(scramble);
  w.nul_string(database);
  w.nul_string(std::string(sha2::plugin_name));
  return w.take();
}

Bytes query(const std::string& sql) {
  Bytes out{command::query};
  out.insert(out.end(), sql.begin(), sql.end());
  return out;
}

std::vector<Packet> packets(const Bytes& bytes) {
  protocol::PacketReader reader;
  reader.feed(bytes);
  std::vector<Packet> out;
  while (auto p = reader.next()) out.push_back(std::move(*p));
  return out;
}

struct Bed {
  boost::asio::io_context io;
  std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>(io);
  catalog::Catalog node;
  txn::Store rows;
  VerifierCache verifiers;
  FakeChecker checker;
  std::shared_ptr<Connection> connection;

  Bed() {
    ConnectionSettings settings;
    settings.server_version = "8.4.13-kvasir";
    settings.connection_id = 7;
    connection = std::make_shared<Connection>(transport, io.get_executor(), settings, registry(), node, rows, verifiers, checker);
    connection->start();
    run();
  }
  // The client goes, so the read it left pending ends and nothing holds the
  // connection.
  ~Bed() {
    transport->hang_up();
    run();
  }
  void run() {
    io.restart();
    io.poll();
  }
  // The nonce from the greeting the connection sent.
  std::array<uint8_t, protocol::nonce_size> nonce() {
    const auto greeting = packets(transport->sent).at(0);
    Reader r(greeting.payload);
    r.u8();
    r.nul_string(64);
    r.u32();
    std::array<uint8_t, protocol::nonce_size> n{};
    const auto first = r.view(8);
    std::copy(first.begin(), first.end(), n.begin());
    r.skip(1 + 2 + 1 + 2 + 2 + 1 + 10);
    const auto second = r.view(12);
    std::copy(second.begin(), second.end(), n.begin() + 8);
    return n;
  }
  // Through TLS and the response, up to the full path's password request.
  void through_tls(const std::string& password) {
    transport->push(framed(ssl_request(), 1));
    run();
    transport->push(framed(response(nonce(), password), 2));
    run();
  }
  std::vector<Packet> sent_after(size_t skip) {
    auto all = packets(transport->sent);
    return {all.begin() + static_cast<std::ptrdiff_t>(std::min(skip, all.size())), all.end()};
  }
};

TEST(ServerConnection, TheGreetingComesFirst) {
  Bed bed;
  const auto sent = packets(bed.transport->sent);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload[0], 10);
  EXPECT_FALSE(bed.connection->signed_in());
}

TEST(ServerConnection, NothingPastTheSslRequestIsReadBeforeTls) {
  Bed bed;
  Bytes bytes = framed(ssl_request(), 1);
  const std::string hello = "\x16\x03\x01 a TLS ClientHello";
  bytes.insert(bytes.end(), hello.begin(), hello.end());
  bed.transport->push(bytes);
  bed.run();
  EXPECT_EQ(bed.transport->tls_started, 1);
  // The ClientHello is still the transport's, for the handshake.
  EXPECT_EQ(bed.transport->unread_at_tls, hello.size());
}

TEST(ServerConnection, TheFullPathSignsInAndServesQueries) {
  Bed bed;
  bed.through_tls("secret");
  auto sent = bed.sent_after(1);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload, (Bytes{0x01, sha2::perform_full_authentication}));

  const std::string password("secret\0", 7);
  bed.transport->push(framed(Bytes(password.begin(), password.end()), 4));
  bed.run();
  ASSERT_EQ(bed.checker.asked.size(), 1u);
  EXPECT_EQ(bed.checker.asked[0].first, "root");
  EXPECT_EQ(bed.checker.asked[0].second, "secret");
  EXPECT_FALSE(bed.connection->signed_in());

  bed.checker.answer(true);
  bed.run();
  EXPECT_TRUE(bed.connection->signed_in());
  sent = bed.sent_after(2);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload[0], 0x00);

  bed.transport->push(framed(query("select @@version_comment limit 1"), 0));
  bed.run();
  sent = bed.sent_after(3);
  ASSERT_EQ(sent.size(), 4u);
  Reader row(sent[2].payload);
  EXPECT_EQ(row.lenenc_string(256), "Kvasir - The Distributed Relational Database (in memory, not durable)");

  bed.transport->push(framed(Bytes{command::quit}, 0));
  bed.run();
  EXPECT_TRUE(bed.transport->closed);
  EXPECT_TRUE(bed.connection->finished());
}

// A command packet with no payload is complete on its header: it is answered
// 1047 "Unknown command" (08S01) and the connection goes on (observed on
// MySQL 8.4.13, four zero bytes written after a PyMySQL sign-in).
TEST(ServerConnection, AnEmptyCommandPacketIsUnknownAndTheConnectionGoesOn) {
  Bed bed;
  bed.through_tls("secret");
  const std::string password("secret\0", 7);
  bed.transport->push(framed(Bytes(password.begin(), password.end()), 4));
  bed.run();
  bed.checker.answer(true);
  bed.run();
  ASSERT_TRUE(bed.connection->signed_in());

  bed.transport->push(framed(Bytes{}, 0));
  bed.run();
  auto sent = bed.sent_after(3);
  ASSERT_EQ(sent.size(), 1u);
  Reader err(sent[0].payload);
  EXPECT_EQ(err.u8(), 0xff);
  EXPECT_EQ(err.u16(), 1047);
  EXPECT_EQ(std::string(sent[0].payload.begin() + 3, sent[0].payload.end()), "#08S01Unknown command");
  EXPECT_EQ(sent[0].first_sequence, 1);

  bed.transport->push(framed(query("select @@version_comment limit 1"), 0));
  bed.run();
  EXPECT_EQ(bed.sent_after(4).size(), 4u);
  EXPECT_FALSE(bed.connection->finished());
}

// CLIENT_CONNECT_WITH_DB (Go's and JDBC's DSNs name one): the session starts
// in the database the response names, and one that is not there is refused
// with 1049 in place of the OK, the connection closed (observed on MySQL
// 8.4.13: "ERROR 1049 (42000): Unknown database 'nope'").
TEST(ServerConnection, TheResponsesDatabase) {
  const auto sign_in = [](Bed& bed, const std::string& database) {
    bed.verifiers.remember("root", sha2::fast_verifier({reinterpret_cast<const uint8_t*>("secret"), 6}));
    bed.transport->push(framed(ssl_request(), 1));
    bed.run();
    bed.transport->push(framed(response_with_database(bed.nonce(), "secret", database), 2));
    bed.run();
  };
  {
    Bed bed;
    exec::Session admin(registry(), bed.node, bed.rows, exec::SessionInfo{"root", "10.0.0.2", 8});
    admin.execute(sql::parse("CREATE DATABASE kv_db").at(0));
    sign_in(bed, "kv_db");
    ASSERT_TRUE(bed.connection->signed_in());
    bed.transport->push(framed(query("SELECT DATABASE()"), 0));
    bed.run();
    const auto sent = bed.sent_after(3);
    ASSERT_EQ(sent.size(), 4u);
    Reader row(sent[2].payload);
    EXPECT_EQ(row.lenenc_string(256), "kv_db");
  }
  Bed bed;
  sign_in(bed, "nope");
  const auto sent = bed.sent_after(1);
  // The fast path's answer, then the refusal where the OK would be.
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].payload, (Bytes{0x01, sha2::fast_auth_success}));
  EXPECT_EQ(sent[1].first_sequence, 4u);
  Reader r(sent[1].payload);
  EXPECT_EQ(r.u8(), 0xff);
  EXPECT_EQ(r.u16(), 1049);
  EXPECT_EQ(r.u8(), '#');
  r.view(5);
  EXPECT_EQ(r.rest_string(256), "Unknown database 'nope'");
  EXPECT_TRUE(bed.transport->closed);
  EXPECT_FALSE(bed.connection->signed_in());
}

// A statement waiting for another transaction's lock keeps its connection,
// though no read is pending then and the node holds no reference of its own
// (found with the real mysql client: the connection was dropped at once),
// and answers when the lock is let go.
TEST(ServerConnection, AStatementWaitsForALockAndKeepsItsConnection) {
  Bed bed;
  bed.verifiers.remember("root", sha2::fast_verifier({reinterpret_cast<const uint8_t*>("secret"), 6}));
  bed.through_tls("secret");
  ASSERT_TRUE(bed.connection->signed_in());
  exec::Session other(registry(), bed.node, bed.rows, exec::SessionInfo{"root", "10.0.0.2", 8});
  const auto run_other = [&](const std::string& sql) {
    for (const auto& s : sql::parse(sql, other.parser_options())) other.execute(s);
  };
  run_other("CREATE DATABASE d; USE d; CREATE TABLE t (a INT PRIMARY KEY); BEGIN; INSERT INTO t VALUES (1)");
  std::weak_ptr<Connection> weak = bed.connection;
  bed.connection.reset();
  const size_t before = packets(bed.transport->sent).size();
  bed.transport->push(framed(query("INSERT INTO d.t VALUES (1)"), 0));
  bed.run();
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(packets(bed.transport->sent).size(), before);
  EXPECT_FALSE(bed.transport->closed);
  run_other("COMMIT");
  bed.run();
  const auto sent = bed.sent_after(before);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload[0], 0xff);
  EXPECT_EQ(sent[0].payload[1] | (sent[0].payload[2] << 8), 1062);
  bed.connection = weak.lock();
  ASSERT_TRUE(bed.connection);
}

TEST(ServerConnection, TheFastPathNeedsNoCheck) {
  Bed bed;
  bed.verifiers.remember("root", sha2::fast_verifier({reinterpret_cast<const uint8_t*>("secret"), 6}));
  bed.through_tls("secret");
  EXPECT_TRUE(bed.connection->signed_in());
  EXPECT_TRUE(bed.checker.asked.empty());
  const auto sent = bed.sent_after(1);
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].payload, (Bytes{0x01, sha2::fast_auth_success}));
}

TEST(ServerConnection, AWrongPasswordIsDeniedAndClosed) {
  Bed bed;
  bed.through_tls("guess");
  const std::string password("guess\0", 6);
  bed.transport->push(framed(Bytes(password.begin(), password.end()), 4));
  bed.run();
  bed.checker.answer(false);
  bed.run();
  const auto sent = bed.sent_after(2);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload[0], 0xff);
  EXPECT_TRUE(bed.transport->closed);
  EXPECT_FALSE(bed.connection->signed_in());
}

TEST(ServerConnection, APlaintextClientIsRefused) {
  Bed bed;
  Writer w;
  w.u32(client_flags & ~capability::ssl);
  w.u32(16 * 1024 * 1024);
  w.u8(255);
  w.zeros(23);
  w.nul_string("root");
  w.lenenc(0);
  w.nul_string(std::string(sha2::plugin_name));
  bed.transport->push(framed(w.take(), 1));
  bed.run();
  const auto sent = bed.sent_after(1);
  ASSERT_EQ(sent.size(), 1u);
  Reader r(sent[0].payload);
  EXPECT_EQ(r.u8(), 0xff);
  EXPECT_EQ(r.u16(), er_secure_transport_required);
  EXPECT_TRUE(bed.transport->closed);
  EXPECT_EQ(bed.transport->tls_started, 0);
}

TEST(ServerConnection, AFailedHandshakeEndsTheConnection) {
  Bed bed;
  bed.transport->tls_fails = true;
  bed.transport->push(framed(ssl_request(), 1));
  bed.run();
  EXPECT_TRUE(bed.transport->aborted);
  EXPECT_TRUE(bed.connection->finished());
}

TEST(ServerConnection, ALargePacketBeforeSignInIsRefusedUnread) {
  Bed bed;
  // A header naming a 1 MiB payload, before anyone has signed in.
  bed.transport->push(Bytes{0x00, 0x00, 0x10, 0x01});
  bed.run();
  const auto sent = bed.sent_after(1);
  ASSERT_EQ(sent.size(), 1u);
  Reader r(sent[0].payload);
  EXPECT_EQ(r.u8(), 0xff);
  EXPECT_EQ(r.u16(), protocol::er_net_packet_too_large);
  EXPECT_TRUE(bed.transport->closed);
  EXPECT_LE(bed.transport->largest_read, 4u);
}

TEST(ServerConnection, AClientThatGoesIsFinished) {
  Bed bed;
  bed.transport->hang_up();
  bed.run();
  EXPECT_TRUE(bed.connection->finished());
  EXPECT_TRUE(bed.transport->aborted);
}

// Found by LeakSanitizer on hosted CI: a connection whose client went while it
// waited for a packet kept itself alive through its own pending continuation.
TEST(ServerConnection, AConnectionIsFreedWhenItsClientGoes) {
  Bed bed;
  bed.verifiers.remember("root", sha2::fast_verifier({reinterpret_cast<const uint8_t*>("secret"), 6}));
  bed.through_tls("secret");
  ASSERT_TRUE(bed.connection->signed_in());
  std::weak_ptr<Connection> watch = bed.connection;
  bed.transport->hang_up();
  bed.run();
  bed.connection.reset();
  EXPECT_TRUE(watch.expired());
}

TEST(ServerConnection, PacketsSplitAcrossReadsAreJoined) {
  Bed bed;
  bed.verifiers.remember("root", sha2::fast_verifier({reinterpret_cast<const uint8_t*>("secret"), 6}));
  bed.transport->push(framed(ssl_request(), 1));
  bed.run();
  const auto bytes = framed(response(bed.nonce(), "secret"), 2);
  for (const uint8_t b : bytes) {
    bed.transport->push(Bytes{b});
    bed.run();
  }
  EXPECT_TRUE(bed.connection->signed_in());
}

}  // namespace
}  // namespace kvasir::server
