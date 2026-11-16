//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/commands.h"

#include <algorithm>
#include <limits>

#include "kvasir/protocol/messages.h"
#include "kvasir/sql/parser.h"

namespace kvasir::server {

namespace capability = protocol::capability;

CommandPhase::CommandPhase(exec::Session& session, uint32_t capabilities) : _session(session), _capabilities(capabilities) {}

void CommandPhase::send(protocol::Bytes& out, const protocol::Bytes& payload) { protocol::frame(payload, _sequence, out); }

uint16_t CommandPhase::warning_count() const {
  return static_cast<uint16_t>(std::min<size_t>(_session.warnings().size(), std::numeric_limits<uint16_t>::max()));
}

void CommandPhase::ok(protocol::Bytes& out, uint64_t affected_rows, uint64_t last_insert_id, const std::string& info) {
  protocol::Ok ok;
  ok.affected_rows = affected_rows;
  ok.last_insert_id = last_insert_id;
  ok.info = info;
  ok.status = _session.status_flags();
  ok.warnings = warning_count();
  send(out, protocol::encode(ok));
}

void CommandPhase::error(protocol::Bytes& out, uint16_t code, const std::string& sqlstate, const std::string& message) {
  send(out, protocol::encode(protocol::Err{code, sqlstate, message}));
}

CommandStep CommandPhase::received(const protocol::Packet& packet) {
  CommandStep step;
  // Every command begins a new exchange at sequence 0.
  _sequence = static_cast<uint8_t>(packet.last_sequence + 1);
  if (packet.first_sequence != 0) {
    error(step.out, protocol::er_net_packets_out_of_order, "08S01", "Got packets out of order");
    step.close = true;
    return step;
  }
  if (packet.payload.empty()) {
    error(step.out, er_unknown_com_error, "08S01", "Unknown command");
    return step;
  }
  const auto body = std::string_view(reinterpret_cast<const char*>(packet.payload.data()) + 1, packet.payload.size() - 1);
  try {
    switch (packet.payload[0]) {
      case command::quit:
        step.close = true;
        return step;
      case command::ping:
        ok(step.out);
        return step;
      case command::query:
        return query(body);
      case command::init_db: {
        sql::Statement use;
        use.kind = sql::Statement::Kind::use;
        use.database = std::string(body);
        _session.execute(use);
        ok(step.out);
        return step;
      }
      case command::reset_connection:
        _session.reset();
        ok(step.out);
        return step;
      default:
        error(step.out, er_unknown_com_error, "08S01", "Unknown command");
        return step;
    }
  } catch (const exec::SqlError& e) {
    error(step.out, e.code(), e.sqlstate(), e.what());
  }
  return step;
}

CommandStep CommandPhase::query(std::string_view text) {
  CommandStep step;
  // A refusal replaces anything begun, from the reply's first sequence.
  const uint8_t first = _sequence;
  const auto refuse = [&](uint16_t code, const std::string& sqlstate, const std::string& message) {
    step.out.clear();
    _sequence = first;
    error(step.out, code, sqlstate, message);
  };
  try {
    const auto statements = sql::parse(text, _session.parser_options());
    // Without CLIENT_MULTI_STATEMENTS, which Kvasir does not offer, a second
    // statement is a syntax error where it begins.
    if (statements.size() > 1) {
      uint32_t line = 1;
      for (size_t i = 0; i < statements[1].offset; ++i)
        if (text[i] == '\n') ++line;
      throw sql::SyntaxError(text, statements[1].offset, line);
    }
    const auto outcome = _session.execute(statements.front());
    if (!outcome.result) {
      ok(step.out, outcome.affected_rows, outcome.last_insert_id, outcome.info);
      return step;
    }
    const auto& result = *outcome.result;
    const bool deprecate_eof = (_capabilities & capability::deprecate_eof) != 0;
    send(step.out, protocol::encode_column_count(result.columns.size()));
    for (const auto& c : result.columns) {
      protocol::ColumnDefinition d;
      d.schema = c.schema;
      d.table = c.table;
      d.original_table = c.original_table;
      d.name = c.name;
      d.original_name = c.original_name;
      d.character_set = c.type.collation;
      d.length = c.type.length;
      d.type = c.type.field_type;
      d.flags = c.type.flags;
      d.decimals = c.type.decimals;
      send(step.out, protocol::encode(d));
    }
    if (!deprecate_eof) send(step.out, protocol::encode_eof(warning_count(), _session.status_flags()));
    for (const auto& row : result.rows) send(step.out, protocol::encode_text_row(row));
    if (deprecate_eof) {
      protocol::Ok end;
      end.as_eof = true;
      end.status = _session.status_flags();
      end.warnings = warning_count();
      send(step.out, protocol::encode(end));
    } else {
      send(step.out, protocol::encode_eof(warning_count(), _session.status_flags()));
    }
  } catch (const sql::SyntaxError& e) {
    _session.record_error(sql::er_parse_error, e.what());
    refuse(sql::er_parse_error, "42000", e.what());
  } catch (const sql::EmptyQuery& e) {
    _session.record_error(sql::er_empty_query, e.what());
    refuse(sql::er_empty_query, "42000", e.what());
  } catch (const exec::SqlError& e) {
    refuse(e.code(), e.sqlstate(), e.what());
  } catch (const exec::LockWaitNeeded& e) {
    // Nothing is sent: the statement runs again from the start once the lock
    // is let go, its reply from this same sequence.
    step.out.clear();
    _sequence = first;
    _pending = Pending{std::string(text), first, e};
    step.waiting = true;
  }
  return step;
}

CommandStep CommandPhase::wait(std::function<void()> wake) {
  CommandStep step;
  if (!_pending) return step;
  if (_session.wait_for_lock(_pending->wait, std::move(wake))) {
    step.waiting = true;
    return step;
  }
  // A deadlock: this transaction was the victim.
  _sequence = _pending->sequence;
  _pending.reset();
  error(step.out, exec::er_lock_deadlock, "40001", "Deadlock found when trying to get lock; try restarting transaction");
  return step;
}

CommandStep CommandPhase::resume() {
  if (!_pending) return {};
  const auto pending = std::move(*_pending);
  _pending.reset();
  _sequence = pending.sequence;
  return query(pending.text);
}

CommandStep CommandPhase::give_up() {
  CommandStep step;
  if (!_pending) return step;
  _session.give_up_waiting(_pending->wait);
  _sequence = _pending->sequence;
  _pending.reset();
  error(step.out, exec::er_lock_wait_timeout, "HY000", "Lock wait timeout exceeded; try restarting transaction");
  return step;
}

}  // namespace kvasir::server
