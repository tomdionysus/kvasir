//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "kvasir/exec/session.h"
#include "kvasir/protocol/codec.h"
#include "kvasir/protocol/packet.h"
#include "kvasir/server/statistics.h"

// The command phase, once a client has signed in, as a state machine with no
// I/O like the sign-in: each packet from the client is a command, and each
// step says what to write back and whether to close. A command's packets
// start at sequence 0 and its reply at 1 (the MySQL 8.4 manual, "Command
// Phase").
//
// Commands: COM_QUIT, COM_INIT_DB, COM_QUERY (one statement: Kvasir does not
// offer CLIENT_MULTI_STATEMENTS), COM_STATISTICS, COM_PING and
// COM_RESET_CONNECTION. Any
// other is refused as MySQL refuses one it does not know. Results go as text
// result sets, ended by EOF or, for a client that asked for
// CLIENT_DEPRECATE_EOF, by OK with the EOF header.
namespace kvasir::server {

namespace command {
inline constexpr uint8_t quit = 0x01;
inline constexpr uint8_t init_db = 0x02;
inline constexpr uint8_t query = 0x03;
inline constexpr uint8_t statistics = 0x09;
inline constexpr uint8_t ping = 0x0e;
inline constexpr uint8_t reset_connection = 0x1f;
}  // namespace command

inline constexpr uint16_t er_unknown_com_error = 1047;

struct CommandStep {
  protocol::Bytes out;
  bool close = false;
  // The statement met another transaction's lock: nothing is sent yet. The
  // caller calls wait(), then resume() once woken, or give_up() at
  // innodb_lock_wait_timeout; no other packet is read meanwhile.
  bool waiting = false;
};

class CommandPhase {
 public:
  // `capabilities` are those both sides agreed at sign-in. `statistics`, the
  // node's, outlives the phase; the sign-in that made it counts as a
  // question. Without it COM_STATISTICS answers with no counts.
  CommandPhase(exec::Session& session, uint32_t capabilities, NodeStatistics* statistics = nullptr);
  ~CommandPhase();
  CommandPhase(const CommandPhase&) = delete;
  CommandPhase& operator=(const CommandPhase&) = delete;

  CommandStep received(const protocol::Packet& packet);

  // A waiting statement waits: `wake` is called once, on whatever thread
  // frees the lock, and must only post. The step is still waiting, or is
  // the deadlock's error (1213, the transaction rolled back).
  CommandStep wait(std::function<void()> wake);
  // Runs the waiting statement again.
  CommandStep resume();
  // The wait took too long: 1205, the statement undone, the transaction
  // kept.
  CommandStep give_up();

 private:
  struct Pending {
    std::string text;
    // The reply's first sequence.
    uint8_t sequence;
    exec::LockWaitNeeded wait;
  };
  CommandStep receive(const protocol::Packet& packet);
  // The node's count of running commands: one begins with each packet, and
  // ends with the step that answers it (a waiting statement still runs).
  void begin_command();
  CommandStep settle(CommandStep step);
  CommandStep query(std::string_view text);
  void send(protocol::Bytes& out, const protocol::Bytes& payload);
  void ok(protocol::Bytes& out, uint64_t affected_rows = 0, uint64_t last_insert_id = 0, const std::string& info = {});
  void error(protocol::Bytes& out, uint16_t code, const std::string& sqlstate, const std::string& message);
  uint16_t warning_count() const;

  exec::Session& _session;
  const uint32_t _capabilities;
  NodeStatistics* const _statistics;
  bool _running = false;
  uint8_t _sequence = 0;
  std::optional<Pending> _pending;
};

}  // namespace kvasir::server
