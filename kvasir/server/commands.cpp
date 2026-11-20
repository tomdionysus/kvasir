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
#include "kvasir/protocol/statements.h"
#include "kvasir/protocol/types.h"
#include "kvasir/sql/parser.h"

namespace kvasir::server {

namespace capability = protocol::capability;

CommandPhase::CommandPhase(exec::Session& session, uint32_t capabilities, NodeStatistics* statistics)
    : _session(session), _capabilities(capabilities), _statistics(statistics) {
  if (_statistics) _statistics->question();
}

// A phase let go of while its statement waits (the client left) runs no
// longer.
CommandPhase::~CommandPhase() { settle({}); }

void CommandPhase::begin_command() {
  if (!_statistics || _running) return;
  _statistics->command_began();
  _running = true;
}

CommandStep CommandPhase::settle(CommandStep step) {
  if (_running && !step.waiting) {
    _statistics->command_ended();
    _running = false;
  }
  return step;
}

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
  begin_command();
  // Every command is a question but these (the manual, "Questions").
  const auto code = packet.payload.empty() ? 0 : packet.payload[0];
  const bool question = packet.payload.empty() || (code != command::ping && code != command::statistics && code != command::stmt_prepare &&
                                                   code != command::stmt_close && code != command::stmt_reset);
  if (_statistics && question) _statistics->question();
  return settle(receive(packet));
}

CommandStep CommandPhase::receive(const protocol::Packet& packet) {
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
      case command::statistics: {
        // One string, no header.
        const std::string text = _statistics ? _statistics->text() : NodeStatistics(dannan::contract::steady_time_source()).text();
        send(step.out, protocol::Bytes(text.begin(), text.end()));
        return step;
      }
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
        _statements.clear();
        ok(step.out);
        return step;
      case command::stmt_prepare:
        return prepare(body);
      case command::stmt_execute:
        return execute(std::span(packet.payload).subspan(1));
      case command::stmt_send_long_data:
        // Never answered, even when refused (the manual).
        send_long_data(std::span(packet.payload).subspan(1));
        return step;
      case command::stmt_close: {
        // Never answered.
        const auto id = protocol::statement_of(std::span(packet.payload).subspan(1));
        _session.close_prepared(id);
        _statements.erase(id);
        return step;
      }
      case command::stmt_reset:
        return reset_statement(std::span(packet.payload).subspan(1));
      default:
        error(step.out, er_unknown_com_error, "08S01", "Unknown command");
        return step;
    }
  } catch (const exec::SqlError& e) {
    error(step.out, e.code(), e.sqlstate(), e.what());
  } catch (const protocol::ProtocolError& e) {
    error(step.out, e.code(), "HY000", e.what());
  }
  return step;
}

void CommandPhase::definitions(protocol::Bytes& out, const std::vector<exec::Column>& columns) {
  for (const auto& c : columns) {
    protocol::ColumnDefinition d;
    d.schema = c.schema;
    d.table = c.table;
    d.original_table = c.original_table;
    d.name = c.name;
    d.original_name = c.original_name;
    d.character_set = c.type.collation;
    d.length = c.type.length;
    d.type = c.type.field_type;
    // MySQL's server never sends NUM; its client library adds it.
    d.flags = static_cast<uint16_t>(c.type.flags & ~protocol::column_flag::num);
    d.decimals = c.type.decimals;
    send(out, protocol::encode(d));
  }
  if (!(_capabilities & capability::deprecate_eof)) send(out, protocol::encode_eof(warning_count(), _session.status_flags()));
}

void CommandPhase::respond(protocol::Bytes& out, const exec::Outcome& outcome, bool binary) {
  if (!outcome.result) {
    ok(out, outcome.affected_rows, outcome.last_insert_id, outcome.info);
    return;
  }
  const auto& result = *outcome.result;
  send(out, protocol::encode_column_count(result.columns.size()));
  definitions(out, result.columns);
  std::vector<protocol::ValueType> types;
  for (const auto& c : result.columns) types.push_back({c.type.field_type, (c.type.flags & protocol::column_flag::unsigned_) != 0});
  for (const auto& row : result.rows) send(out, binary ? protocol::encode_binary_row(types, row) : protocol::encode_text_row(row));
  if (_capabilities & capability::deprecate_eof) {
    protocol::Ok end;
    end.as_eof = true;
    end.status = _session.status_flags();
    end.warnings = warning_count();
    send(out, protocol::encode(end));
  } else {
    send(out, protocol::encode_eof(warning_count(), _session.status_flags()));
  }
}

CommandStep CommandPhase::prepare(std::string_view text) {
  CommandStep step;
  const uint8_t first = _sequence;
  const auto refuse = [&](uint16_t code, const std::string& sqlstate, const std::string& message) {
    step.out.clear();
    _sequence = first;
    error(step.out, code, sqlstate, message);
  };
  try {
    auto options = _session.parser_options();
    options.placeholders = true;
    auto statements = sql::parse(text, options);
    if (statements.size() > 1) {
      uint32_t line = 1;
      for (size_t i = 0; i < statements[1].offset; ++i)
        if (text[i] == '\n') ++line;
      throw sql::SyntaxError(text, statements[1].offset, line);
    }
    const auto p = _session.prepare(std::move(statements.front()));
    if (p.params.size() > std::numeric_limits<uint16_t>::max()) {
      _session.close_prepared(p.id);
      throw exec::SqlError(1390, "HY000", "Prepared statement contains too many placeholders");
    }
    send(step.out,
         protocol::encode(protocol::PrepareOk{p.id, static_cast<uint16_t>(p.columns.size()), static_cast<uint16_t>(p.params.size()), warning_count()}));
    if (!p.params.empty()) definitions(step.out, p.params);
    if (!p.columns.empty()) definitions(step.out, p.columns);
    _statements[p.id] =
        Statement{std::vector<std::optional<protocol::ValueType>>(p.params.size()), std::vector<std::optional<std::string>>(p.params.size()), 0};
  } catch (const sql::SyntaxError& e) {
    _session.record_error(sql::er_parse_error, e.what());
    refuse(sql::er_parse_error, "42000", e.what());
  } catch (const sql::EmptyQuery& e) {
    _session.record_error(sql::er_empty_query, e.what());
    refuse(sql::er_empty_query, "42000", e.what());
  } catch (const exec::SqlError& e) {
    refuse(e.code(), e.sqlstate(), e.what());
  }
  return step;
}

CommandStep CommandPhase::execute(std::span<const uint8_t> body) {
  CommandStep step;
  try {
    const auto id = protocol::statement_of(body);
    const auto count = _session.parameter_types(id, "mysqld_stmt_execute").size();
    auto& kept = _statements[id];
    kept.bound.resize(count);
    kept.long_data.resize(count);
    // What was sent ahead is used once, whatever the execute's fate.
    auto long_data = std::move(kept.long_data);
    const bool too_long = kept.long_bytes > max_long_data_bytes;
    kept.long_data.assign(count, std::nullopt);
    kept.long_bytes = 0;
    if (too_long) throw exec::SqlError(protocol::er_net_packet_too_large, "08S01", "Got a packet bigger than 'max_allowed_packet' bytes");
    const auto e = protocol::decode_execute(body, kept.bound, long_data);
    if (e.flags != 0) throw exec::SqlError(exec::er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'cursors'");
    return run_prepared(id, _session.bind(id, e.params));
  } catch (const exec::SqlError& e) {
    error(step.out, e.code(), e.sqlstate(), e.what());
  } catch (const protocol::ProtocolError& e) {
    error(step.out, e.code(), "HY000", e.what());
  }
  return step;
}

CommandStep CommandPhase::run_prepared(uint32_t id, std::vector<exec::Parameter> parameters) {
  CommandStep step;
  const uint8_t first = _sequence;
  try {
    const auto outcome = _session.execute_prepared(id, parameters);
    respond(step.out, outcome, true);
  } catch (const exec::SqlError& e) {
    step.out.clear();
    _sequence = first;
    error(step.out, e.code(), e.sqlstate(), e.what());
  } catch (const exec::LockWaitNeeded& e) {
    step.out.clear();
    _sequence = first;
    _pending = Pending{std::string(), first, e, id, std::move(parameters)};
    step.waiting = true;
  }
  return step;
}

void CommandPhase::send_long_data(std::span<const uint8_t> body) {
  // Too short to name a statement and a parameter: nothing to keep.
  if (body.size() < 6) return;
  protocol::Reader r(body);
  const auto id = r.u32();
  const auto param = r.u16();
  const auto data = r.view(r.remaining());
  const auto it = _statements.find(id);
  if (it == _statements.end() || param >= it->second.long_data.size()) return;
  auto& kept = it->second;
  // Past max_allowed_packet nothing more is kept, and the execute is refused.
  kept.long_bytes += data.size();
  if (kept.long_bytes > max_long_data_bytes) return;
  auto& value = kept.long_data[param];
  if (!value) value.emplace();
  value->append(reinterpret_cast<const char*>(data.data()), data.size());
}

CommandStep CommandPhase::reset_statement(std::span<const uint8_t> body) {
  CommandStep step;
  const auto id = protocol::statement_of(body);
  const auto count = _session.parameter_types(id, "mysqld_stmt_reset").size();
  auto& kept = _statements[id];
  kept.long_data.assign(count, std::nullopt);
  kept.long_bytes = 0;
  ok(step.out);
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
    respond(step.out, _session.execute(statements.front()), false);
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
    _pending = Pending{std::string(text), first, e, std::nullopt, {}};
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
  return settle(std::move(step));
}

CommandStep CommandPhase::resume() {
  if (!_pending) return {};
  auto pending = std::move(*_pending);
  _pending.reset();
  _sequence = pending.sequence;
  if (pending.prepared) return settle(run_prepared(*pending.prepared, std::move(pending.parameters)));
  return settle(query(pending.text));
}

CommandStep CommandPhase::give_up() {
  CommandStep step;
  if (!_pending) return step;
  _session.give_up_waiting(_pending->wait);
  _sequence = _pending->sequence;
  _pending.reset();
  error(step.out, exec::er_lock_wait_timeout, "HY000", "Lock wait timeout exceeded; try restarting transaction");
  return settle(std::move(step));
}

}  // namespace kvasir::server
