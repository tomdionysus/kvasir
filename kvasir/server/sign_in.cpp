//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/sign_in.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "dannan/identity/crypto.h"
#include "kvasir/protocol/caching_sha2.h"

namespace kvasir::server {

using dannan::contract::Lock;
using dannan::identity::Secret;
namespace capability = protocol::capability;
namespace sha2 = protocol::caching_sha2;

namespace {

// utf8mb4_0900_ai_ci, MySQL 8.4's default collation.
inline constexpr uint8_t default_character_set = 255;

uint32_t offered(bool tls) {
  uint32_t capabilities = capability::long_password | capability::long_flag | capability::connect_with_db | capability::protocol_41 | capability::transactions |
                          capability::secure_connection | capability::plugin_auth | capability::plugin_auth_lenenc_client_data | capability::connect_attrs |
                          capability::deprecate_eof;
  if (tls) capabilities |= capability::ssl;
  return capabilities;
}

const char* sqlstate_for(uint16_t code) {
  switch (code) {
    case protocol::er_handshake_error:
    case protocol::er_net_packet_too_large:
    case protocol::er_net_packets_out_of_order:
      return "08S01";
    default:
      return "HY000";
  }
}

}  // namespace

VerifierCache::VerifierCache(size_t capacity) : _capacity(capacity) {
  if (capacity == 0) throw std::invalid_argument("a verifier cache holds at least one verifier");
}

VerifierCache::Check VerifierCache::check(const std::string& user, std::span<const uint8_t> scramble,
                                          std::span<const uint8_t, protocol::nonce_size> nonce) const {
  Lock lock(_mutex);
  const auto found = _verifiers.find(user);
  if (found == _verifiers.end()) return Check::absent;
  return sha2::check_scramble(scramble, nonce, found->second) ? Check::matched : Check::mismatched;
}

void VerifierCache::remember(const std::string& user, Secret verifier) {
  Lock lock(_mutex);
  if (auto found = _verifiers.find(user); found != _verifiers.end()) {
    found->second = std::move(verifier);
    _order.erase(std::find(_order.begin(), _order.end(), user));
    _order.push_back(user);
    return;
  }
  if (_verifiers.size() >= _capacity) {
    _verifiers.erase(_order.front());
    _order.pop_front();
  }
  _verifiers.emplace(user, std::move(verifier));
  _order.push_back(user);
}

void VerifierCache::forget(const std::string& user) {
  Lock lock(_mutex);
  if (_verifiers.erase(user) == 0) return;
  _order.erase(std::find(_order.begin(), _order.end(), user));
}

void VerifierCache::clear() {
  Lock lock(_mutex);
  _verifiers.clear();
  _order.clear();
}

size_t VerifierCache::size() const {
  Lock lock(_mutex);
  return _verifiers.size();
}

namespace {
// The back-off's keys: opaque to it, Lugh's style.
std::string principal_key(const std::string& user) { return "principal:" + user; }
std::string source_key(const std::string& host) { return "source:" + host; }
}  // namespace

std::array<uint8_t, protocol::nonce_size> make_nonce() {
  std::array<uint8_t, protocol::nonce_size> nonce{};
  const auto drawn = dannan::identity::random_bytes(nonce.size());
  std::copy(drawn.begin(), drawn.end(), nonce.begin());
  for (auto& byte : nonce)
    while (byte == 0) byte = dannan::identity::random_bytes(1)[0];
  return nonce;
}

SignIn::SignIn(SignInSettings settings, VerifierCache& verifiers, const SignInBackoff* backoff)
    : _settings(std::move(settings)), _verifiers(verifiers), _backoff(backoff), _server_capabilities(offered(_settings.tls_available)) {}

void SignIn::send(Bytes& out, const Bytes& payload) { protocol::frame(payload, _sequence, out); }

SignInStep SignIn::greeting() {
  SignInStep step;
  if (_state != State::start) return refuse(protocol::er_handshake_error, "08S01", "Bad handshake");
  protocol::Handshake handshake;
  handshake.server_version = _settings.server_version;
  handshake.connection_id = _settings.connection_id;
  handshake.nonce = _settings.nonce;
  handshake.capabilities = _server_capabilities;
  handshake.character_set = default_character_set;
  handshake.status = protocol::status::autocommit;
  handshake.auth_plugin = std::string(sha2::plugin_name);
  send(step.out, protocol::encode(handshake));
  _state = State::greeted;
  return step;
}

SignInStep SignIn::received(const protocol::Packet& packet) {
  if (_state == State::closed) return closing();
  if (packet.first_sequence != _sequence) return refuse(protocol::er_net_packets_out_of_order, "08S01", "Got packets out of order");
  _sequence = static_cast<uint8_t>(packet.last_sequence + 1);
  try {
    switch (_state) {
      case State::greeted:
      case State::awaiting_response:
        return response(packet);
      case State::awaiting_switch:
        return authenticate(packet.payload);
      case State::awaiting_password: {
        // Asking for the RSA key is refused, but is no guess at a password.
        if (packet.payload.size() == 1 && packet.payload[0] == sha2::request_public_key) return deny(false);
        auto password = sha2::read_password(packet.payload);
        if (password.size() == 0) return deny();
        _pending_verifier = sha2::fast_verifier(password.reveal());
        _state = State::checking;
        SignInStep step;
        step.check_password = PasswordCheck{_user, std::move(password)};
        return step;
      }
      default:
        return refuse(protocol::er_handshake_error, "08S01", "Bad handshake");
    }
  } catch (const protocol::ProtocolError& error) {
    return refuse(error.code(), sqlstate_for(error.code()), error.what());
  }
}

SignInStep SignIn::response(const protocol::Packet& packet) {
  const auto answer = protocol::decode_handshake_response(packet.payload, _server_capabilities);
  if (answer.ssl_request) {
    if (_state != State::greeted) return refuse(protocol::er_handshake_error, "08S01", "Bad handshake");
    _state = State::tls_pending;
    SignInStep step;
    step.start_tls = true;
    return step;
  }
  // Nothing a client sends in plaintext is acted on.
  if (_state == State::greeted)
    return refuse(er_secure_transport_required, "HY000", "Connections using insecure transport are prohibited while --require_secure_transport=ON.");

  _user = answer.user;
  _database = answer.database;
  _client_capabilities = answer.capabilities;
  _client_collation = answer.character_set;
  if (!answer.auth_plugin) return refuse(protocol::er_handshake_error, "08S01", "Bad handshake");
  if (*answer.auth_plugin != sha2::plugin_name) {
    Bytes data(_settings.nonce.begin(), _settings.nonce.end());
    data.push_back(0);
    SignInStep step;
    send(step.out, protocol::encode_auth_switch(std::string(sha2::plugin_name), data));
    _state = State::awaiting_switch;
    return step;
  }
  return authenticate(answer.auth_response);
}

SignInStep SignIn::authenticate(std::span<const uint8_t> scramble) {
  _password_given = !scramble.empty();
  // Before anything is hashed. Being turned away is not another failure.
  if (waiting()) return deny(false);
  if (scramble.empty()) return deny();
  switch (_verifiers.check(_user, scramble, _settings.nonce)) {
    case VerifierCache::Check::matched: {
      Bytes out;
      const uint8_t success[] = {sha2::fast_auth_success};
      send(out, protocol::encode_auth_more_data(success));
      return accept(std::move(out));
    }
    case VerifierCache::Check::mismatched:
      return deny();
    case VerifierCache::Check::absent:
      break;
  }
  SignInStep step;
  const uint8_t full[] = {sha2::perform_full_authentication};
  send(step.out, protocol::encode_auth_more_data(full));
  _state = State::awaiting_password;
  return step;
}

SignInStep SignIn::tls_up() {
  if (_state != State::tls_pending) return refuse(protocol::er_handshake_error, "08S01", "Bad handshake");
  _state = State::awaiting_response;
  return {};
}

SignInStep SignIn::checked(bool matches) {
  if (_state != State::checking) return closing();
  if (!matches) return deny();
  _verifiers.remember(_user, std::move(_pending_verifier));
  _pending_verifier = Secret();
  return accept({});
}

SignInStep SignIn::accept(Bytes out) {
  if (_backoff) _backoff->principals.succeeded(principal_key(_user));
  protocol::Ok ok;
  ok.status = protocol::status::autocommit;
  send(out, protocol::encode(ok));
  _state = State::signed_in;
  SignInStep step;
  step.out = std::move(out);
  return step;
}

bool SignIn::waiting() const {
  if (!_backoff) return false;
  const auto now = _backoff->time.now();
  return _backoff->principals.wait(principal_key(_user), now) || _backoff->sources.wait(source_key(_settings.peer_host), now);
}

SignInStep SignIn::deny(bool failure) {
  if (failure && _backoff) {
    const auto now = _backoff->time.now();
    _backoff->principals.failed(principal_key(_user), now);
    _backoff->sources.failed(source_key(_settings.peer_host), now);
  }
  return refuse(er_access_denied, "28000",
                "Access denied for user '" + _user + "'@'" + _settings.peer_host + "' (using password: " + (_password_given ? "YES" : "NO") + ")");
}

SignInStep SignIn::closing() {
  _state = State::closed;
  _pending_verifier = Secret();
  SignInStep step;
  step.close = true;
  return step;
}

SignInStep SignIn::refuse(uint16_t code, const char* sqlstate, const std::string& message) {
  SignInStep step;
  send(step.out, protocol::encode(protocol::Err{code, sqlstate, message}));
  step.close = true;
  _state = State::closed;
  _pending_verifier = Secret();
  return step;
}

}  // namespace kvasir::server
