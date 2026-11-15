//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/secure_memory.h"
#include "kvasir/protocol/codec.h"
#include "kvasir/protocol/messages.h"
#include "kvasir/protocol/packet.h"

// A connection's sign-in, the MySQL protocol's connection phase, as a state
// machine with no I/O: the owner feeds it the client's packets and the
// transport's events, and each step says what to write, whether to upgrade to
// TLS, whether a password is to be checked, and whether to close. So it is
// tested without a socket, and the TLS upgrade is one seam for the owner.
//
// The rules (the design, "Compatibility and authentication"):
// - TLS before any authentication. A client that answers the greeting in
//   plaintext rather than asking for TLS is refused, as MySQL refuses it under
//   require_secure_transport, and nothing it sent is acted on.
// - caching_sha2_password only. A client that offers another method is asked
//   to switch to it; nothing switches away from it.
// - The fast path uses a verifier held in memory only (VerifierCache), made
//   from an earlier full sign-in. The full path takes the password over TLS
//   and hands it to the owner to check against the account; the RSA exchange
//   is never offered.
// - An empty password never signs in.
// - Every refusal of credentials is the same ERR, whether or not the user
//   exists; nothing here logs a typed name or any authentication data.
namespace kvasir::server {

using protocol::Bytes;

inline constexpr uint16_t er_access_denied = 1045;
inline constexpr uint16_t er_secure_transport_required = 3159;

// The fast path's verifiers, SHA256(SHA256(password)) by user name, in secure
// memory and nowhere else. Bounded: past `capacity` the oldest is dropped,
// which costs that user one full sign-in. Names are compared as bytes.
// Thread-safe; waits only on its own lock, never on I/O.
class VerifierCache {
 public:
  // Throws std::invalid_argument for a capacity of 0 (discipline 5: a bound
  // that cannot hold one verifier is not a bound).
  explicit VerifierCache(size_t capacity = 4096);

  enum class Check { absent, matched, mismatched };
  // Checks a client's scramble against the user's verifier, under its lock,
  // so a verifier never leaves the cache.
  Check check(const std::string& user, std::span<const uint8_t> scramble, std::span<const uint8_t, protocol::nonce_size> nonce) const;
  void remember(const std::string& user, dannan::identity::Secret verifier);
  // Drops the user's verifier: on any change to the principal (its password,
  // its removal, a lock).
  void forget(const std::string& user);
  void clear();
  size_t size() const;

 private:
  const size_t _capacity;
  mutable dannan::contract::Mutex _mutex;
  std::map<std::string, dannan::identity::Secret> _verifiers DANNAN_GUARDED_BY(_mutex);
  // Names in the order they were remembered, oldest first.
  std::deque<std::string> _order DANNAN_GUARDED_BY(_mutex);
};

struct SignInSettings {
  // As the greeting reports it: "8.4.<n>-kvasir".
  std::string server_version;
  uint32_t connection_id = 0;
  // The client's address, as an access-denied message names it.
  std::string peer_host;
  // Whether the listener holds certificates. Without them nobody can sign in,
  // since TLS is required, but the greeting is still sent and each client
  // refused in MySQL's words.
  bool tls_available = true;
  // The greeting's nonce: twenty random bytes with no NUL among them, since
  // the auth switch request carries it NUL-ended (make_nonce()). Given here
  // so a test can fix it.
  std::array<uint8_t, protocol::nonce_size> nonce{};
};

// Twenty random bytes from Dannan's generator, none of them NUL.
std::array<uint8_t, protocol::nonce_size> make_nonce();

// What the owner does after a step, in this order: write `out`; then, if
// `start_tls`, take the server side of the TLS handshake and call tls_up();
// if `check_password`, check it against the account named and call checked();
// if `close`, close once `out` is written.
struct PasswordCheck {
  std::string user;
  dannan::identity::Secret password;
};

struct SignInStep {
  Bytes out;
  bool start_tls = false;
  std::optional<PasswordCheck> check_password;
  bool close = false;
};

class SignIn {
 public:
  SignIn(SignInSettings settings, VerifierCache& verifiers);

  // The greeting, the connection's first packet.
  SignInStep greeting();
  // A whole packet from the client.
  SignInStep received(const protocol::Packet& packet);
  // The TLS handshake asked for has completed.
  SignInStep tls_up();
  // The answer to a check_password: whether the password is the account's.
  SignInStep checked(bool matches);

  bool signed_in() const noexcept { return _state == State::signed_in; }
  bool closed() const noexcept { return _state == State::closed; }
  // Set once signed in.
  const std::string& user() const noexcept { return _user; }
  const std::optional<std::string>& database() const noexcept { return _database; }
  uint32_t client_capabilities() const noexcept { return _client_capabilities; }
  // The capabilities offered, which with the client's govern the session.
  uint32_t server_capabilities() const noexcept { return _server_capabilities; }
  // The sequence id the next packet the server sends takes.
  uint8_t sequence() const noexcept { return _sequence; }

 private:
  enum class State { start, greeted, tls_pending, awaiting_response, awaiting_switch, awaiting_password, checking, signed_in, closed };

  SignInStep response(const protocol::Packet& packet);
  SignInStep authenticate(std::span<const uint8_t> scramble);
  SignInStep accept(Bytes out);
  SignInStep deny();
  // Closes with nothing more to say: after a refusal, or for an owner that
  // called out of turn.
  SignInStep closing();
  SignInStep refuse(uint16_t code, const char* sqlstate, const std::string& message);
  void send(Bytes& out, const Bytes& payload);

  const SignInSettings _settings;
  VerifierCache& _verifiers;
  const uint32_t _server_capabilities;
  State _state = State::start;
  uint8_t _sequence = 0;
  std::string _user;
  std::optional<std::string> _database;
  uint32_t _client_capabilities = 0;
  // Whether the client sent a password, as an access-denied message says.
  bool _password_given = false;
  // Made from the password the full path received, kept until it is checked.
  dannan::identity::Secret _pending_verifier;
};

}  // namespace kvasir::server
