//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "kvasir/exec/session.h"
#include "kvasir/protocol/codec.h"
#include "kvasir/protocol/packet.h"

// The command phase, once a client has signed in, as a state machine with no
// I/O like the sign-in: each packet from the client is a command, and each
// step says what to write back and whether to close. A command's packets
// start at sequence 0 and its reply at 1 (the MySQL 8.4 manual, "Command
// Phase").
//
// Commands: COM_QUIT, COM_INIT_DB, COM_QUERY (one statement: Kvasir does not
// offer CLIENT_MULTI_STATEMENTS), COM_PING and COM_RESET_CONNECTION. Any
// other is refused as MySQL refuses one it does not know. Results go as text
// result sets, ended by EOF or, for a client that asked for
// CLIENT_DEPRECATE_EOF, by OK with the EOF header.
namespace kvasir::server {

namespace command {
inline constexpr uint8_t quit = 0x01;
inline constexpr uint8_t init_db = 0x02;
inline constexpr uint8_t query = 0x03;
inline constexpr uint8_t ping = 0x0e;
inline constexpr uint8_t reset_connection = 0x1f;
}  // namespace command

inline constexpr uint16_t er_unknown_com_error = 1047;

struct CommandStep {
  protocol::Bytes out;
  bool close = false;
};

class CommandPhase {
 public:
  // `capabilities` are those both sides agreed at sign-in.
  CommandPhase(exec::Session& session, uint32_t capabilities);

  CommandStep received(const protocol::Packet& packet);

 private:
  CommandStep query(std::string_view text);
  void send(protocol::Bytes& out, const protocol::Bytes& payload);
  void ok(protocol::Bytes& out, uint64_t affected_rows = 0, uint64_t last_insert_id = 0);
  void error(protocol::Bytes& out, uint16_t code, const std::string& sqlstate, const std::string& message);
  uint16_t warning_count() const;

  exec::Session& _session;
  const uint32_t _capabilities;
  uint8_t _sequence = 0;
};

}  // namespace kvasir::server
