//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual, "Caching SHA-2 Pluggable Authentication" and the client/
// server protocol's caching_sha2_password exchange:
// - the client's scramble is SHA256(password) XOR
//   SHA256(SHA256(SHA256(password)) || nonce), 32 bytes; an empty password
//   sends no bytes;
// - the server holds SHA256(SHA256(password)) for the fast path;
// - the full path over TLS carries the password followed by a NUL;
// - AuthMoreData's status bytes: 0x02 asks for the public key, 0x03 is fast
//   authentication's success, 0x04 asks for full authentication.
// The known answer for "secret" under the nonce 01..14 was computed with the
// openssl command line on a build Pi, not with this code.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "dannan/encoding/base.h"
#include "dannan/identity/crypto.h"
#include "kvasir/protocol/caching_sha2.h"

namespace kvasir::protocol::caching_sha2 {
namespace {

std::span<const uint8_t> as_bytes(std::string_view text) { return {reinterpret_cast<const uint8_t*>(text.data()), text.size()}; }

std::array<uint8_t, nonce_size> counting_nonce() {
  std::array<uint8_t, nonce_size> nonce{};
  for (size_t i = 0; i < nonce.size(); ++i) nonce[i] = static_cast<uint8_t>(i + 1);
  return nonce;
}

std::string hex(std::span<const uint8_t> bytes) { return dannan::encoding::hex(bytes); }

TEST(CachingSha2, StatusBytesAreTheManuals) {
  EXPECT_EQ(plugin_name, "caching_sha2_password");
  EXPECT_EQ(request_public_key, 0x02);
  EXPECT_EQ(fast_auth_success, 0x03);
  EXPECT_EQ(perform_full_authentication, 0x04);
}

TEST(CachingSha2, ScrambleMatchesTheKnownAnswer) {
  const auto nonce = counting_nonce();
  EXPECT_EQ(hex(scramble(as_bytes("secret"), nonce)), "746ebe205d56a0707acb3e796e834e0dd7b1d61743b26bd5202c7a623230c7c9");
}

TEST(CachingSha2, AnEmptyPasswordSendsNoScramble) {
  const auto nonce = counting_nonce();
  EXPECT_TRUE(scramble({}, nonce).empty());
}

TEST(CachingSha2, VerifierIsTheDoubleHash) {
  EXPECT_EQ(hex(fast_verifier(as_bytes("secret")).reveal()), "3881219d087dd9c634373fd33dfa33a2cb6bfc6c520b64b8bb60ef2ceb534ae7");
}

TEST(CachingSha2, TheRightPasswordPassesTheFastPath) {
  const auto nonce = counting_nonce();
  EXPECT_TRUE(check_scramble(scramble(as_bytes("secret"), nonce), nonce, fast_verifier(as_bytes("secret"))));
}

TEST(CachingSha2, AWrongPasswordFailsTheFastPath) {
  const auto nonce = counting_nonce();
  EXPECT_FALSE(check_scramble(scramble(as_bytes("Secret"), nonce), nonce, fast_verifier(as_bytes("secret"))));
}

TEST(CachingSha2, AScrambleIsBoundToItsNonce) {
  const auto nonce = counting_nonce();
  auto other = nonce;
  other[0] ^= 0x80;
  EXPECT_FALSE(check_scramble(scramble(as_bytes("secret"), nonce), other, fast_verifier(as_bytes("secret"))));
}

TEST(CachingSha2, AScrambleOfTheWrongSizeFails) {
  const auto nonce = counting_nonce();
  auto good = scramble(as_bytes("secret"), nonce);
  const auto verifier = fast_verifier(as_bytes("secret"));
  EXPECT_FALSE(check_scramble({}, nonce, verifier));
  EXPECT_FALSE(check_scramble(std::span<const uint8_t>(good).first(31), nonce, verifier));
  good.push_back(0);
  EXPECT_FALSE(check_scramble(good, nonce, verifier));
}

TEST(CachingSha2, AVerifierOfTheWrongSizeFails) {
  const auto nonce = counting_nonce();
  EXPECT_FALSE(check_scramble(scramble(as_bytes("secret"), nonce), nonce, dannan::identity::Secret()));
}

TEST(CachingSha2, ReadsThePasswordBeforeItsNul) {
  const std::string packet("secret\0", 7);
  const auto password = read_password(as_bytes(packet));
  EXPECT_EQ(std::string(password.reveal().begin(), password.reveal().end()), "secret");
}

TEST(CachingSha2, ReadsAnEmptyPassword) {
  const std::string packet("\0", 1);
  EXPECT_EQ(read_password(as_bytes(packet)).size(), 0u);
}

TEST(CachingSha2, RefusesAPasswordWithoutItsNul) {
  try {
    read_password(as_bytes("secret"));
    FAIL() << "read a password with no NUL";
  } catch (const ProtocolError& error) {
    EXPECT_EQ(error.code(), er_malformed_packet);
  }
  EXPECT_THROW(read_password({}), ProtocolError);
}

TEST(CachingSha2, RefusesBytesAfterTheNul) {
  const std::string packet("sec\0ret\0", 8);
  EXPECT_THROW(read_password(as_bytes(packet)), ProtocolError);
}

TEST(CachingSha2, RefusesAPasswordPastItsBound) {
  std::string packet(max_password_bytes + 1, 'p');
  packet.push_back('\0');
  EXPECT_THROW(read_password(as_bytes(packet)), ProtocolError);
  packet.erase(0, 1);
  EXPECT_EQ(read_password(as_bytes(packet)).size(), max_password_bytes);
}

}  // namespace
}  // namespace kvasir::protocol::caching_sha2
