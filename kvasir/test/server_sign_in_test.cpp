//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual and the client/server protocol's connection phase:
// - the server's greeting is sequence 0; the client's SSLRequest is 1, and
//   after TLS its HandshakeResponse41 is 2; each reply takes the next id;
// - caching_sha2_password: fast authentication's success is AuthMoreData
//   0x03 followed by OK; a request for full authentication is AuthMoreData
//   0x04, answered over TLS by the password and a NUL;
// - a client whose plugin differs is sent AuthSwitchRequest (0xfe, the
//   plugin's name, the nonce NUL-ended) and answers with the plugin's data;
// - ER_ACCESS_DENIED_ERROR 1045 (28000): "Access denied for user '%s'@'%s'
//   (using password: %s)", NO for an empty password;
// - ER_SECURE_TRANSPORT_REQUIRED 3159 (HY000) for a plaintext client under
//   require_secure_transport;
// - ER_HANDSHAKE_ERROR 1043 (08S01) "Bad handshake", ER_NET_PACKETS_OUT_OF_ORDER
//   1156 (08S01) "Got packets out of order".
// And the design: TLS before authentication, caching_sha2_password only, no
// RSA exchange, verifiers in memory only, an empty password never signs in.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "kvasir/protocol/caching_sha2.h"
#include "kvasir/protocol/codec.h"
#include "kvasir/protocol/messages.h"
#include "kvasir/protocol/packet.h"
#include "kvasir/server/sign_in.h"

namespace kvasir::server {
namespace {

using protocol::Packet;
using protocol::Reader;
using protocol::Writer;
namespace capability = protocol::capability;
namespace sha2 = protocol::caching_sha2;

std::span<const uint8_t> as_bytes(std::string_view text) { return {reinterpret_cast<const uint8_t*>(text.data()), text.size()}; }

SignInSettings settings() {
  SignInSettings s;
  s.server_version = "8.4.0-kvasir";
  s.connection_id = 7;
  s.peer_host = "10.0.0.1";
  for (size_t i = 0; i < s.nonce.size(); ++i) s.nonce[i] = static_cast<uint8_t>(i + 1);
  return s;
}

// The packets in `bytes`, each with its sequence id.
std::vector<Packet> packets(const Bytes& bytes) {
  protocol::PacketReader reader;
  reader.feed(bytes);
  std::vector<Packet> out;
  while (auto p = reader.next()) out.push_back(std::move(*p));
  EXPECT_EQ(reader.buffered(), 0u);
  return out;
}

Packet packet(uint8_t sequence, Bytes payload) { return Packet{sequence, sequence, std::move(payload)}; }

inline constexpr uint32_t client_flags = capability::protocol_41 | capability::secure_connection | capability::plugin_auth |
                                         capability::plugin_auth_lenenc_client_data | capability::connect_with_db | capability::long_password |
                                         capability::transactions | capability::deprecate_eof;

Bytes ssl_request(uint32_t flags = client_flags | capability::ssl) {
  Writer w;
  w.u32(flags);
  w.u32(16 * 1024 * 1024);
  w.u8(255);
  w.zeros(23);
  return w.take();
}

Bytes handshake_response(const std::string& user, const Bytes& auth, const std::string& plugin = std::string(sha2::plugin_name),
                         uint32_t flags = client_flags | capability::ssl, const std::string& database = "shop") {
  Writer w;
  w.u32(flags);
  w.u32(16 * 1024 * 1024);
  w.u8(255);
  w.zeros(23);
  w.nul_string(user);
  w.lenenc(auth.size());
  w.raw(auth);
  w.nul_string(database);
  w.nul_string(plugin);
  return w.take();
}

Bytes scramble_for(const std::string& password) { return sha2::scramble(as_bytes(password), settings().nonce); }

struct ErrFields {
  uint16_t code = 0;
  std::string sqlstate;
  std::string message;
};
ErrFields err(const Packet& p) {
  Reader r(p.payload);
  EXPECT_EQ(r.u8(), 0xff);
  ErrFields e;
  e.code = r.u16();
  EXPECT_EQ(r.u8(), '#');
  const auto state = r.view(5);
  e.sqlstate.assign(state.begin(), state.end());
  e.message = r.rest_string(1024);
  return e;
}

bool is_ok(const Packet& p) { return !p.payload.empty() && p.payload[0] == 0x00; }

// A client through the greeting and TLS, about to send its response.
struct Bed {
  VerifierCache cache;
  SignIn sign_in{settings(), cache};
  Bed() {
    sign_in.greeting();
    const auto asked = sign_in.received(packet(1, ssl_request()));
    EXPECT_TRUE(asked.start_tls);
    EXPECT_TRUE(asked.out.empty());
    EXPECT_FALSE(asked.close);
    const auto up = sign_in.tls_up();
    EXPECT_TRUE(up.out.empty());
  }
};

TEST(ServerSignIn, TheGreetingOffersCachingSha2AndTls) {
  VerifierCache cache;
  SignIn sign_in(settings(), cache);
  const auto step = sign_in.greeting();
  const auto sent = packets(step.out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 0);
  Reader r(sent[0].payload);
  EXPECT_EQ(r.u8(), 10);
  EXPECT_EQ(r.nul_string(64), "8.4.0-kvasir");
  EXPECT_EQ(r.u32(), 7u);
  const auto first = r.view(8);
  EXPECT_EQ(first[0], 1);
  EXPECT_EQ(first[7], 8);
  r.skip(1);
  uint32_t capabilities = r.u16();
  r.skip(3);
  capabilities |= static_cast<uint32_t>(r.u16()) << 16;
  EXPECT_TRUE(capabilities & capability::ssl);
  EXPECT_TRUE(capabilities & capability::plugin_auth);
  EXPECT_TRUE(capabilities & capability::secure_connection);
  EXPECT_TRUE(capabilities & capability::protocol_41);
  EXPECT_EQ(capabilities, sign_in.server_capabilities());
  r.skip(1 + 10 + 12 + 1);
  EXPECT_EQ(r.nul_string(64), "caching_sha2_password");
  EXPECT_FALSE(step.close);
}

TEST(ServerSignIn, WithoutCertificatesTlsIsNotOffered) {
  VerifierCache cache;
  auto s = settings();
  s.tls_available = false;
  SignIn sign_in(s, cache);
  sign_in.greeting();
  EXPECT_FALSE(sign_in.server_capabilities() & capability::ssl);
}

TEST(ServerSignIn, APlaintextClientIsRefusedAndNothingIsChecked) {
  VerifierCache cache;
  SignIn sign_in(settings(), cache);
  sign_in.greeting();
  const auto step = sign_in.received(packet(1, handshake_response("root", scramble_for("secret"), std::string(sha2::plugin_name), client_flags)));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.start_tls);
  EXPECT_FALSE(step.check_password);
  const auto sent = packets(step.out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 2);
  const auto e = err(sent[0]);
  EXPECT_EQ(e.code, er_secure_transport_required);
  EXPECT_EQ(e.sqlstate, "HY000");
  EXPECT_EQ(e.message, "Connections using insecure transport are prohibited while --require_secure_transport=ON.");
  EXPECT_FALSE(sign_in.signed_in());
  EXPECT_TRUE(sign_in.closed());
}

TEST(ServerSignIn, WithoutCertificatesEveryClientIsRefused) {
  VerifierCache cache;
  auto s = settings();
  s.tls_available = false;
  SignIn sign_in(s, cache);
  sign_in.greeting();
  const auto step = sign_in.received(packet(1, handshake_response("root", scramble_for("secret"), std::string(sha2::plugin_name), client_flags)));
  EXPECT_TRUE(step.close);
  EXPECT_EQ(err(packets(step.out).at(0)).code, er_secure_transport_required);
}

TEST(ServerSignIn, TheFullPathAsksForThePasswordAndRemembersItsVerifier) {
  Bed bed;
  const auto asked = bed.sign_in.received(packet(2, handshake_response("root", scramble_for("secret"))));
  auto sent = packets(asked.out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 3);
  EXPECT_EQ(sent[0].payload, (Bytes{0x01, sha2::perform_full_authentication}));
  EXPECT_FALSE(asked.check_password);

  const std::string password("secret\0", 7);
  auto check = bed.sign_in.received(packet(4, Bytes(password.begin(), password.end())));
  EXPECT_TRUE(check.out.empty());
  ASSERT_TRUE(check.check_password);
  EXPECT_EQ(check.check_password->user, "root");
  const auto revealed = check.check_password->password.reveal();
  EXPECT_EQ(std::string(revealed.begin(), revealed.end()), "secret");
  EXPECT_FALSE(bed.sign_in.signed_in());

  const auto done = bed.sign_in.checked(true);
  sent = packets(done.out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 5);
  EXPECT_TRUE(is_ok(sent[0]));
  EXPECT_FALSE(done.close);
  EXPECT_TRUE(bed.sign_in.signed_in());
  EXPECT_EQ(bed.sign_in.user(), "root");
  EXPECT_EQ(bed.sign_in.database(), "shop");
  // The collation the response asked for, which the session takes.
  EXPECT_EQ(bed.sign_in.client_collation(), 255);
  EXPECT_EQ(bed.sign_in.sequence(), 6);
  EXPECT_EQ(bed.cache.check("root", scramble_for("secret"), settings().nonce), VerifierCache::Check::matched);
}

TEST(ServerSignIn, AFailedFullPathIsDeniedAndRemembersNothing) {
  Bed bed;
  bed.sign_in.received(packet(2, handshake_response("root", scramble_for("guess"))));
  const std::string password("guess\0", 6);
  bed.sign_in.received(packet(4, Bytes(password.begin(), password.end())));
  const auto done = bed.sign_in.checked(false);
  EXPECT_TRUE(done.close);
  const auto sent = packets(done.out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 5);
  const auto e = err(sent[0]);
  EXPECT_EQ(e.code, er_access_denied);
  EXPECT_EQ(e.sqlstate, "28000");
  EXPECT_EQ(e.message, "Access denied for user 'root'@'10.0.0.1' (using password: YES)");
  EXPECT_FALSE(bed.sign_in.signed_in());
  EXPECT_EQ(bed.cache.size(), 0u);
}

TEST(ServerSignIn, TheFastPathSignsInWithNoPasswordSent) {
  Bed bed;
  bed.cache.remember("root", sha2::fast_verifier(as_bytes("secret")));
  const auto step = bed.sign_in.received(packet(2, handshake_response("root", scramble_for("secret"))));
  EXPECT_FALSE(step.check_password);
  EXPECT_FALSE(step.close);
  const auto sent = packets(step.out);
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].first_sequence, 3);
  EXPECT_EQ(sent[0].payload, (Bytes{0x01, sha2::fast_auth_success}));
  EXPECT_EQ(sent[1].first_sequence, 4);
  EXPECT_TRUE(is_ok(sent[1]));
  EXPECT_TRUE(bed.sign_in.signed_in());
}

TEST(ServerSignIn, AWrongScrambleOnTheFastPathIsDenied) {
  Bed bed;
  bed.cache.remember("root", sha2::fast_verifier(as_bytes("secret")));
  const auto step = bed.sign_in.received(packet(2, handshake_response("root", scramble_for("Secret"))));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.check_password);
  const auto e = err(packets(step.out).at(0));
  EXPECT_EQ(e.code, er_access_denied);
  EXPECT_EQ(e.message, "Access denied for user 'root'@'10.0.0.1' (using password: YES)");
}

TEST(ServerSignIn, AnEmptyPasswordNeverSignsIn) {
  Bed bed;
  const auto step = bed.sign_in.received(packet(2, handshake_response("root", Bytes{})));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.check_password);
  const auto e = err(packets(step.out).at(0));
  EXPECT_EQ(e.code, er_access_denied);
  EXPECT_EQ(e.message, "Access denied for user 'root'@'10.0.0.1' (using password: NO)");
}

TEST(ServerSignIn, AnUnknownUserIsAskedForItsPasswordAsAnyOtherIs) {
  Bed bed;
  bed.cache.remember("root", sha2::fast_verifier(as_bytes("secret")));
  const auto step = bed.sign_in.received(packet(2, handshake_response("nobody", scramble_for("secret"))));
  EXPECT_EQ(packets(step.out).at(0).payload, (Bytes{0x01, sha2::perform_full_authentication}));
}

TEST(ServerSignIn, AnotherPluginIsSwitchedToCachingSha2) {
  Bed bed;
  bed.cache.remember("root", sha2::fast_verifier(as_bytes("secret")));
  const auto switched = bed.sign_in.received(packet(2, handshake_response("root", Bytes(20, 0x33), "mysql_native_password")));
  EXPECT_FALSE(switched.close);
  auto sent = packets(switched.out);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].first_sequence, 3);
  Reader r(sent[0].payload);
  EXPECT_EQ(r.u8(), 0xfe);
  EXPECT_EQ(r.nul_string(64), "caching_sha2_password");
  const auto nonce = r.view(protocol::nonce_size);
  const auto expected = settings().nonce;
  EXPECT_EQ(std::vector<uint8_t>(nonce.begin(), nonce.end()), std::vector<uint8_t>(expected.begin(), expected.end()));
  EXPECT_EQ(r.u8(), 0);
  EXPECT_EQ(r.remaining(), 0u);

  const auto step = bed.sign_in.received(packet(4, scramble_for("secret")));
  sent = packets(step.out);
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].first_sequence, 5);
  EXPECT_EQ(sent[0].payload, (Bytes{0x01, sha2::fast_auth_success}));
  EXPECT_EQ(sent[1].first_sequence, 6);
  EXPECT_TRUE(is_ok(sent[1]));
  EXPECT_TRUE(bed.sign_in.signed_in());
}

TEST(ServerSignIn, AskingForTheRsaKeyIsDenied) {
  Bed bed;
  bed.sign_in.received(packet(2, handshake_response("root", scramble_for("secret"))));
  const auto step = bed.sign_in.received(packet(4, Bytes{sha2::request_public_key}));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.check_password);
  EXPECT_EQ(err(packets(step.out).at(0)).code, er_access_denied);
}

TEST(ServerSignIn, AMalformedPasswordPacketIsRefused) {
  Bed bed;
  bed.sign_in.received(packet(2, handshake_response("root", scramble_for("secret"))));
  const auto step = bed.sign_in.received(packet(4, Bytes{'s', 'e', 'c'}));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.check_password);
  const auto e = err(packets(step.out).at(0));
  EXPECT_EQ(e.code, protocol::er_malformed_packet);
  EXPECT_EQ(e.sqlstate, "HY000");
}

TEST(ServerSignIn, APacketOutOfSequenceIsRefused) {
  Bed bed;
  const auto step = bed.sign_in.received(packet(3, handshake_response("root", scramble_for("secret"))));
  EXPECT_TRUE(step.close);
  const auto e = err(packets(step.out).at(0));
  EXPECT_EQ(e.code, protocol::er_net_packets_out_of_order);
  EXPECT_EQ(e.sqlstate, "08S01");
  EXPECT_EQ(e.message, "Got packets out of order");
}

TEST(ServerSignIn, AClientWithoutProtocol41IsABadHandshake) {
  VerifierCache cache;
  SignIn sign_in(settings(), cache);
  sign_in.greeting();
  const auto step = sign_in.received(packet(1, ssl_request(capability::ssl)));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.start_tls);
  const auto e = err(packets(step.out).at(0));
  EXPECT_EQ(e.code, protocol::er_handshake_error);
  EXPECT_EQ(e.sqlstate, "08S01");
  EXPECT_EQ(e.message, "Bad handshake");
}

TEST(ServerSignIn, ASecondSslRequestAfterTlsIsABadHandshake) {
  Bed bed;
  const auto step = bed.sign_in.received(packet(2, ssl_request()));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.start_tls);
  EXPECT_EQ(err(packets(step.out).at(0)).code, protocol::er_handshake_error);
}

TEST(ServerSignIn, NothingIsActedOnOnceClosed) {
  Bed bed;
  bed.sign_in.received(packet(2, handshake_response("root", Bytes{})));
  ASSERT_TRUE(bed.sign_in.closed());
  const auto step = bed.sign_in.received(packet(4, scramble_for("secret")));
  EXPECT_TRUE(step.out.empty());
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.check_password);
  EXPECT_FALSE(bed.sign_in.checked(true).check_password);
  EXPECT_FALSE(bed.sign_in.signed_in());
}

TEST(ServerSignIn, AnAnswerToNoQuestionClosesTheConnection) {
  Bed bed;
  const auto step = bed.sign_in.checked(true);
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(bed.sign_in.signed_in());
}

TEST(ServerSignIn, NoncesHaveNoNul) {
  std::set<std::vector<uint8_t>> seen;
  for (int i = 0; i < 200; ++i) {
    const auto nonce = make_nonce();
    for (const auto byte : nonce) ASSERT_NE(byte, 0);
    seen.emplace(nonce.begin(), nonce.end());
  }
  EXPECT_EQ(seen.size(), 200u);
}

// Observed on MySQL 8.4.13 (2026-11-20, 300 greetings): every nonce byte is
// from 1 to 127 and never '$', the other 126 values all appearing.
// Connector/J reads the nonce as ASCII text, so a byte past 127 became '?'
// and every Connector/J sign-in to Kvasir was refused.
TEST(ServerSignIn, NoncesAreMySqlsAscii) {
  std::set<uint8_t> seen;
  for (int i = 0; i < 300; ++i)
    for (const auto byte : make_nonce()) {
      ASSERT_GE(byte, 1);
      ASSERT_LE(byte, 127);
      ASSERT_NE(byte, '$');
      seen.insert(byte);
    }
  EXPECT_EQ(seen.size(), 126u);
}

// The design: back-off per principal and per source before any hashing,
// with Dannan's Backoff (a growing wait, never a lock); a waiting client is
// answered as a wrong password is, at once.
struct GuardedBed {
  dannan::test::SteppedTime time;
  dannan::listen::Backoff principals{{2, std::chrono::seconds(1), std::chrono::minutes(15), std::chrono::hours(1)}};
  dannan::listen::Backoff sources{{3, std::chrono::seconds(1), std::chrono::minutes(15), std::chrono::hours(1)}};
  SignInBackoff backoff{principals, sources, time};
  VerifierCache cache;

  // A new connection's sign-in, through the greeting and TLS.
  std::unique_ptr<SignIn> connect(const std::string& host = "10.0.0.1") {
    auto s = settings();
    s.peer_host = host;
    auto sign_in = std::make_unique<SignIn>(s, cache, &backoff);
    sign_in->greeting();
    sign_in->received(packet(1, ssl_request()));
    sign_in->tls_up();
    return sign_in;
  }
  // A full-path attempt with `password`, answered `matches`; the last step.
  SignInStep attempt(const std::string& user, const std::string& password, bool matches, const std::string& host = "10.0.0.1") {
    auto sign_in = connect(host);
    auto asked = sign_in->received(packet(2, handshake_response(user, scramble_for(password))));
    if (asked.close) return asked;
    const std::string sent = password + std::string(1, '\0');
    auto check = sign_in->received(packet(4, Bytes(sent.begin(), sent.end())));
    if (!check.check_password) return check;
    return sign_in->checked(matches);
  }
};

TEST(ServerSignIn, RepeatedFailuresMakeAPrincipalWaitBeforeAnyHashing) {
  GuardedBed bed;
  EXPECT_TRUE(bed.attempt("root", "guess1", false).close);
  EXPECT_TRUE(bed.attempt("root", "guess2", false).close);
  // Two failures: root waits a second. The right password is denied at once,
  // with no full-path request and nothing to hash.
  auto sign_in = bed.connect();
  const auto step = sign_in->received(packet(2, handshake_response("root", scramble_for("secret"))));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.check_password);
  const auto sent = packets(step.out);
  ASSERT_EQ(sent.size(), 1u);
  const auto e = err(sent[0]);
  EXPECT_EQ(e.code, er_access_denied);
  EXPECT_EQ(e.message, "Access denied for user 'root'@'10.0.0.1' (using password: YES)");
  // Being turned away while waiting is not another failure: once the wait
  // has passed the next attempt is heard.
  bed.time.advance(std::chrono::seconds(1));
  const auto done = bed.attempt("root", "secret", true);
  EXPECT_FALSE(done.close);
  EXPECT_TRUE(is_ok(packets(done.out).at(0)));
}

TEST(ServerSignIn, ASourceThatKeepsFailingWaitsWhicheverNameItTries) {
  GuardedBed bed;
  bed.attempt("a", "x", false);
  bed.attempt("b", "x", false);
  bed.attempt("c", "x", false);
  // Three failures from one address: it waits, even for a name with none.
  auto sign_in = bed.connect();
  const auto step = sign_in->received(packet(2, handshake_response("d", scramble_for("x"))));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(step.check_password);
  // Another address is not held back.
  auto other = bed.connect("10.0.0.2");
  EXPECT_FALSE(other->received(packet(2, handshake_response("d", scramble_for("x")))).close);
}

TEST(ServerSignIn, SuccessClearsThePrincipalButNotTheSource) {
  GuardedBed bed;
  bed.attempt("root", "guess", false);
  EXPECT_EQ(bed.principals.size(), 1u);
  bed.attempt("root", "secret", true);
  EXPECT_EQ(bed.principals.size(), 0u);
  EXPECT_EQ(bed.sources.size(), 1u);
}

TEST(ServerSignIn, FastPathAndEmptyPasswordFailuresCount) {
  GuardedBed bed;
  bed.cache.remember("root", sha2::fast_verifier(as_bytes("secret")));
  auto sign_in = bed.connect();
  EXPECT_TRUE(sign_in->received(packet(2, handshake_response("root", scramble_for("Secret")))).close);
  sign_in = bed.connect();
  EXPECT_TRUE(sign_in->received(packet(2, handshake_response("root", Bytes{}))).close);
  // Two failures: even the fast path's right scramble waits.
  sign_in = bed.connect();
  const auto step = sign_in->received(packet(2, handshake_response("root", scramble_for("secret"))));
  EXPECT_TRUE(step.close);
  EXPECT_FALSE(sign_in->signed_in());
}

TEST(VerifierCache, ChecksWithoutRevealing) {
  VerifierCache cache;
  const auto nonce = settings().nonce;
  EXPECT_EQ(cache.check("root", scramble_for("secret"), nonce), VerifierCache::Check::absent);
  cache.remember("root", sha2::fast_verifier(as_bytes("secret")));
  EXPECT_EQ(cache.check("root", scramble_for("secret"), nonce), VerifierCache::Check::matched);
  EXPECT_EQ(cache.check("root", scramble_for("other"), nonce), VerifierCache::Check::mismatched);
}

TEST(VerifierCache, NamesAreComparedAsBytes) {
  VerifierCache cache;
  cache.remember("root", sha2::fast_verifier(as_bytes("secret")));
  EXPECT_EQ(cache.check("Root", scramble_for("secret"), settings().nonce), VerifierCache::Check::absent);
  EXPECT_EQ(cache.check("root ", scramble_for("secret"), settings().nonce), VerifierCache::Check::absent);
}

TEST(VerifierCache, ForgetDropsAPrincipal) {
  VerifierCache cache;
  cache.remember("root", sha2::fast_verifier(as_bytes("secret")));
  cache.remember("app", sha2::fast_verifier(as_bytes("other")));
  cache.forget("root");
  EXPECT_EQ(cache.check("root", scramble_for("secret"), settings().nonce), VerifierCache::Check::absent);
  EXPECT_EQ(cache.size(), 1u);
  cache.clear();
  EXPECT_EQ(cache.size(), 0u);
}

TEST(VerifierCache, RememberingAgainReplaces) {
  VerifierCache cache(2);
  cache.remember("root", sha2::fast_verifier(as_bytes("old")));
  cache.remember("root", sha2::fast_verifier(as_bytes("new")));
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_EQ(cache.check("root", scramble_for("new"), settings().nonce), VerifierCache::Check::matched);
  cache.remember("a", sha2::fast_verifier(as_bytes("a")));
  EXPECT_EQ(cache.size(), 2u);
}

TEST(VerifierCache, PastCapacityTheOldestGoes) {
  VerifierCache cache(2);
  cache.remember("a", sha2::fast_verifier(as_bytes("a")));
  cache.remember("b", sha2::fast_verifier(as_bytes("b")));
  cache.remember("c", sha2::fast_verifier(as_bytes("c")));
  EXPECT_EQ(cache.size(), 2u);
  EXPECT_EQ(cache.check("a", scramble_for("a"), settings().nonce), VerifierCache::Check::absent);
  EXPECT_EQ(cache.check("b", scramble_for("b"), settings().nonce), VerifierCache::Check::matched);
  EXPECT_EQ(cache.check("c", scramble_for("c"), settings().nonce), VerifierCache::Check::matched);
}

TEST(VerifierCache, ACapacityOfZeroIsRefused) { EXPECT_THROW(VerifierCache(0), std::invalid_argument); }

}  // namespace
}  // namespace kvasir::server
