//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "dannan/identity/secure_memory.h"
#include "kvasir/protocol/codec.h"
#include "kvasir/protocol/messages.h"

// caching_sha2_password, the one authentication method Kvasir speaks (the
// MySQL 8.4 manual, "Caching SHA-2 Pluggable Authentication"). The client
// answers the greeting's 20-byte nonce with a scramble:
//
//   SHA256(password) XOR SHA256(SHA256(SHA256(password)) || nonce)
//
// The fast path checks it against SHA256(SHA256(password)), a verifier the
// server holds in memory only, from an earlier full sign-in. The full path,
// taken when there is no verifier, has the client send the password itself,
// ended by a NUL; Kvasir takes it only over TLS, and never offers the RSA
// exchange that would carry it over plaintext. An empty password is an empty
// scramble. Hashing goes through Dannan's primitives.
namespace kvasir::protocol::caching_sha2 {

inline constexpr std::string_view plugin_name = "caching_sha2_password";

// The second byte of the AuthMoreData the server sends during the exchange,
// and the one the client sends to ask for the server's RSA key.
inline constexpr uint8_t request_public_key = 0x02;
inline constexpr uint8_t fast_auth_success = 0x03;
inline constexpr uint8_t perform_full_authentication = 0x04;

inline constexpr size_t scramble_size = 32;
// The longest password the full path reads.
inline constexpr size_t max_password_bytes = max_auth_response_bytes;

// The client's answer: 32 bytes, or none for an empty password.
Bytes scramble(std::span<const uint8_t> password, std::span<const uint8_t, nonce_size> nonce);

// SHA256(SHA256(password)): what the fast path checks against.
dannan::identity::Secret fast_verifier(std::span<const uint8_t> password);

// Whether `scramble` was made from the password `verifier` came from, under
// `nonce`. Constant time in its comparison; false for a scramble of the wrong
// size, and for an empty one (an empty password never takes the fast path).
bool check_scramble(std::span<const uint8_t> scramble, std::span<const uint8_t, nonce_size> nonce, const dannan::identity::Secret& verifier);

// The full path's packet: the password then one NUL, into secure memory.
// Throws ProtocolError (er_malformed_packet) without the NUL, with bytes after
// it, or longer than max_password_bytes.
dannan::identity::Secret read_password(std::span<const uint8_t> payload);

}  // namespace kvasir::protocol::caching_sha2
