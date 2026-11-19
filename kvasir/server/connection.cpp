//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/connection.h"

#include <boost/asio/post.hpp>
#include <chrono>
#include <utility>

#include "kvasir/protocol/messages.h"

namespace kvasir::server {

namespace {

SignInSettings sign_in_settings(const ConnectionSettings& settings, const Transport& transport) {
  SignInSettings s;
  s.server_version = settings.server_version;
  s.connection_id = settings.connection_id;
  s.peer_host = transport.peer_host();
  s.tls_available = transport.tls_available();
  s.nonce = make_nonce();
  return s;
}

std::string as_string(const protocol::Bytes& bytes) { return std::string(bytes.begin(), bytes.end()); }

}  // namespace

Connection::Connection(std::shared_ptr<Transport> transport, boost::asio::any_io_executor executor, ConnectionSettings settings,
                       const exec::VariableRegistry& registry, catalog::Catalog& catalog, txn::Store& rows, VerifierCache& verifiers, PasswordChecker& checker,
                       const SignInBackoff* backoff, NodeStatistics* statistics)
    : _transport(std::move(transport)),
      _executor(std::move(executor)),
      _settings(std::move(settings)),
      _registry(registry),
      _catalog(catalog),
      _rows(rows),
      _checker(checker),
      _statistics(statistics),
      _sign_in(sign_in_settings(_settings, *_transport), verifiers, backoff),
      _reader(_settings.limits),
      _lock_timer(_executor) {
  _reader.restrict(true);
}

void Connection::start() { sign_in_step(_sign_in.greeting()); }

void Connection::read_exactly(size_t count, std::function<void()> then) {
  if (_finished) return;
  _buffer.assign(count, '\0');
  _filled = 0;
  _then = std::move(then);
  continue_read();
}

// One read at a time, each for no more than the bytes still wanted.
void Connection::continue_read() {
  if (_finished) return;
  if (_filled == _buffer.size()) {
    auto then = std::move(_then);
    then();
    return;
  }
  const std::span<uint8_t> rest(reinterpret_cast<uint8_t*>(_buffer.data()) + _filled, _buffer.size() - _filled);
  _transport->read_some(rest, [self = shared_from_this()](std::optional<size_t> bytes) {
    if (self->_finished) return;
    if (!bytes || *bytes == 0) {
      // The client went, or the transport failed: nobody to tell.
      self->_transport->abort();
      self->finish(false);
      return;
    }
    self->_filled += *bytes;
    self->continue_read();
  });
}

void Connection::read_header() {
  read_exactly(4, [self = shared_from_this()] {
    const auto* header = reinterpret_cast<const uint8_t*>(self->_buffer.data());
    const size_t length = static_cast<size_t>(header[0]) | (static_cast<size_t>(header[1]) << 8) | (static_cast<size_t>(header[2]) << 16);
    const uint8_t sequence = header[3];
    try {
      self->_reader.feed({header, 4});
      // The reader judges the header before any payload is read.
      if (auto packet = self->_reader.next()) {
        self->_packet = std::move(*packet);
        self->packet_arrived();
        return;
      }
    } catch (const protocol::ProtocolError& e) {
      protocol::Bytes out;
      uint8_t reply = static_cast<uint8_t>(sequence + 1);
      protocol::frame(protocol::encode(protocol::Err{e.code(), e.code() == protocol::er_malformed_packet ? "HY000" : "08S01", e.what()}), reply, out);
      self->_transport->send(as_string(out));
      self->_transport->close();
      self->finish(false);
      return;
    }
    self->read_payload(length);
  });
}

void Connection::read_payload(size_t length) {
  read_exactly(length, [self = shared_from_this()] {
    try {
      self->_reader.feed({reinterpret_cast<const uint8_t*>(self->_buffer.data()), self->_buffer.size()});
      if (auto packet = self->_reader.next()) {
        self->_packet = std::move(*packet);
        self->packet_arrived();
        return;
      }
    } catch (const protocol::ProtocolError&) {
      self->_transport->abort();
      self->finish(false);
      return;
    }
    // A fragment of a payload of 16 MiB or more: the next header follows.
    self->read_header();
  });
}

void Connection::packet_arrived() {
  if (!_session) {
    sign_in_step(_sign_in.received(_packet));
    return;
  }
  command_step(_commands->received(_packet));
}

void Connection::command_step(CommandStep step) {
  if (_finished) return;
  if (step.waiting) {
    wait_for_lock();
    return;
  }
  if (!step.out.empty()) _transport->send(as_string(step.out));
  if (step.close) {
    _transport->close();
    finish(true);
    return;
  }
  read_header();
}

// A statement waits for another transaction's lock: until it is let go
// (woken, then run again) or innodb_lock_wait_timeout passes (1205). Nothing
// is read meanwhile. The wake comes on whatever thread let the lock go, and
// only posts here.
void Connection::wait_for_lock() {
  std::weak_ptr<Connection> weak = shared_from_this();
  _waiting = true;
  auto step = _commands->wait([weak, executor = _executor] {
    boost::asio::post(executor, [weak] {
      if (auto self = weak.lock()) self->lock_released();
    });
  });
  if (!step.waiting) {
    _waiting = false;
    command_step(std::move(step));
    return;
  }
  // No read is pending while it waits: the timer holds the connection, until
  // it fires or the wake cancels it.
  _lock_timer.expires_after(std::chrono::seconds(static_cast<int64_t>(_session->lock_wait_timeout())));
  _lock_timer.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
    if (!error) self->lock_timed_out();
  });
}

void Connection::lock_released() {
  if (_finished || !_waiting) return;
  _waiting = false;
  _lock_timer.cancel();
  command_step(_commands->resume());
}

void Connection::lock_timed_out() {
  if (_finished || !_waiting) return;
  _waiting = false;
  command_step(_commands->give_up());
}

void Connection::sign_in_step(SignInStep step) {
  if (_finished) return;
  if (!step.out.empty()) _transport->send(as_string(step.out));
  if (step.close) {
    _transport->close();
    finish(false);
    return;
  }
  if (step.start_tls) {
    _transport->start_tls([self = shared_from_this()](std::string error) {
      if (self->_finished) return;
      if (!error.empty()) {
        self->_transport->abort();
        self->finish(false);
        return;
      }
      self->sign_in_step(self->_sign_in.tls_up());
    });
    return;
  }
  if (step.check_password) {
    auto check = std::move(*step.check_password);
    _checker.check(std::move(check.user), std::move(check.password), _executor, [self = shared_from_this()](bool matches) {
      if (self->_finished) return;
      self->sign_in_step(self->_sign_in.checked(matches));
    });
    return;
  }
  if (_sign_in.signed_in() && !_session) {
    _session =
        std::make_unique<exec::Session>(_registry, _catalog, _rows, exec::SessionInfo{_sign_in.user(), _transport->peer_host(), _settings.connection_id});
    _commands = std::make_unique<CommandPhase>(*_session, _sign_in.client_capabilities() & _sign_in.server_capabilities(), _statistics);
    _reader.restrict(false);
  }
  read_header();
}

// A continuation still waiting holds the connection itself: dropped here, or a
// connection whose client went mid-read would keep itself for ever (found by
// LeakSanitizer).
void Connection::finish(bool) {
  _finished = true;
  _then = nullptr;
  _lock_timer.cancel();
}

}  // namespace kvasir::server
