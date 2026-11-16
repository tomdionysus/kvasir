//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "dannan/identity/secure_memory.h"
#include "kvasir/exec/session.h"
#include "kvasir/protocol/packet.h"
#include "kvasir/server/commands.h"
#include "kvasir/server/sign_in.h"

// One client connection: the sign-in and then the command phase, over a
// transport. The transport is Dannan's stream in the node and a fake in
// tests; the TLS upgrade is its start_tls(), MySQL's in-place upgrade with no
// plaintext answer.
//
// Every packet is read exactly: its four-byte header, then the length the
// header names, so nothing past the SSLRequest is consumed before TLS begins
// (the client's ClientHello follows it at once) and a header that names more
// than the connection allows is refused before its bytes are read. Before
// sign-in a payload may be at most PacketLimits::restricted_payload.
//
// A full-path password is checked by a PasswordChecker away from the
// connection (Argon2id spends tens of milliseconds and megabytes, never on a
// connection's strand); its answer comes back on the connection's executor.
// Everything else runs on that executor; not thread-safe otherwise.
namespace kvasir::server {

class Transport {
 public:
  virtual ~Transport() = default;
  // Reads at most `buffer.size()` bytes into `buffer`; `done` on the
  // connection's executor with the bytes read, or nothing when the
  // transport failed or closed. The buffer stays alive until then.
  virtual void read_some(std::span<uint8_t> buffer, std::function<void(std::optional<size_t> bytes)> done) = 0;
  // Queues bytes to be written, in order, without waiting.
  virtual void send(std::string bytes) = 0;
  // Takes the server side of a TLS handshake now: no plaintext answer is
  // written first. `done` on the connection's executor, empty on success, else
  // why.
  virtual void start_tls(std::function<void(std::string)> done) = 0;
  // Closes once what was queued is written.
  virtual void close() = 0;
  // Closes at once.
  virtual void abort() = 0;
  // The client's address as an access-denied message names it.
  virtual std::string peer_host() const = 0;
  virtual bool tls_available() const = 0;
};

class PasswordChecker {
 public:
  virtual ~PasswordChecker() = default;
  // Whether `password` is `user`'s, decided away from the caller; `done` is
  // posted to `executor`, never called inline.
  virtual void check(std::string user, dannan::identity::Secret password, boost::asio::any_io_executor executor, std::function<void(bool)> done) = 0;
};

struct ConnectionSettings {
  std::string server_version;
  uint32_t connection_id = 0;
  protocol::PacketLimits limits;
};

class Connection : public std::enable_shared_from_this<Connection> {
 public:
  // `registry`, `catalog`, `verifiers` and `checker` are the node's and
  // outlive the connection.
  Connection(std::shared_ptr<Transport> transport, boost::asio::any_io_executor executor, ConnectionSettings settings, const exec::VariableRegistry& registry,
             catalog::Catalog& catalog, VerifierCache& verifiers, PasswordChecker& checker);

  // Sends the greeting and serves the client until either side ends. On the
  // connection's executor; returns at once.
  void start();

  bool signed_in() const noexcept { return _session != nullptr; }
  bool finished() const noexcept { return _finished; }

 private:
  void read_header();
  void read_payload(size_t length);
  // Reads exactly `count` bytes into _buffer, then `then`.
  void read_exactly(size_t count, std::function<void()> then);
  void continue_read();
  // _packet has arrived whole.
  void packet_arrived();
  void sign_in_step(SignInStep step);
  void finish(bool clean);

  std::shared_ptr<Transport> _transport;
  boost::asio::any_io_executor _executor;
  ConnectionSettings _settings;
  const exec::VariableRegistry& _registry;
  catalog::Catalog& _catalog;
  PasswordChecker& _checker;
  SignIn _sign_in;
  protocol::PacketReader _reader;
  std::unique_ptr<exec::Session> _session;
  std::unique_ptr<CommandPhase> _commands;
  std::string _buffer;
  size_t _filled = 0;
  std::function<void()> _then;
  protocol::Packet _packet;
  bool _finished = false;
};

}  // namespace kvasir::server
