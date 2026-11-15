//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "kvasir/protocol/codec.h"

// The payloads of the MySQL client/server protocol's connection phase and of
// its generic replies, as the MySQL 8.4 manual lays them out: the server's
// greeting (HandshakeV10), the client's answer (HandshakeResponse41, or the
// 32-byte SSLRequest that precedes a TLS upgrade), OK, ERR and EOF, the
// authentication exchange's packets, and a text result set's column
// definitions and rows. Encoders build what the server sends; the one decoder
// reads what a client sends before it has signed in, so it bounds every field.
// Only protocol 4.1 is spoken: a client without CLIENT_PROTOCOL_41 is refused.
namespace kvasir::protocol {

namespace capability {
inline constexpr uint32_t long_password = 1u << 0;
inline constexpr uint32_t found_rows = 1u << 1;
inline constexpr uint32_t long_flag = 1u << 2;
inline constexpr uint32_t connect_with_db = 1u << 3;
inline constexpr uint32_t protocol_41 = 1u << 9;
inline constexpr uint32_t ssl = 1u << 11;
inline constexpr uint32_t transactions = 1u << 13;
inline constexpr uint32_t secure_connection = 1u << 15;
inline constexpr uint32_t multi_statements = 1u << 16;
inline constexpr uint32_t multi_results = 1u << 17;
inline constexpr uint32_t plugin_auth = 1u << 19;
inline constexpr uint32_t connect_attrs = 1u << 20;
inline constexpr uint32_t plugin_auth_lenenc_client_data = 1u << 21;
inline constexpr uint32_t session_track = 1u << 23;
inline constexpr uint32_t deprecate_eof = 1u << 24;
}  // namespace capability

namespace status {
inline constexpr uint16_t in_transaction = 0x0001;
inline constexpr uint16_t autocommit = 0x0002;
}  // namespace status

inline constexpr size_t nonce_size = 20;

// Bounds on what a client may send before it has signed in.
inline constexpr size_t max_user_bytes = 256;
inline constexpr size_t max_auth_response_bytes = 1024;
inline constexpr size_t max_database_bytes = 256;
inline constexpr size_t max_plugin_bytes = 256;
inline constexpr size_t max_attributes_bytes = 64 * 1024;

// The server's greeting, protocol version 10.
struct Handshake {
  std::string server_version;
  uint32_t connection_id = 0;
  std::array<uint8_t, nonce_size> nonce{};
  uint32_t capabilities = 0;
  uint8_t character_set = 0;
  uint16_t status = 0;
  std::string auth_plugin;
};
Bytes encode(const Handshake& handshake);

// The client's answer to the greeting.
struct HandshakeResponse {
  uint32_t capabilities = 0;  // as the client sent them
  uint32_t max_packet = 0;
  uint8_t character_set = 0;
  // True for the 32-byte SSLRequest: the fields below it are then empty, and
  // the whole response follows once TLS is up.
  bool ssl_request = false;
  std::string user;
  Bytes auth_response;
  std::optional<std::string> database;
  std::optional<std::string> auth_plugin;
  std::vector<std::pair<std::string, std::string>> attributes;
};
// Reads a client's answer. A field is read only when both the client and
// `server_capabilities` name its capability. Throws ProtocolError
// (er_handshake_error, "Bad handshake") for a client that does not speak
// protocol 4.1 or secure authentication, and er_malformed_packet for bytes
// that do not fit the layout or its bounds.
HandshakeResponse decode_handshake_response(std::span<const uint8_t> payload, uint32_t server_capabilities);

// OK. With `as_eof` its header is 0xfe, which ends a result set for a client
// that asked for CLIENT_DEPRECATE_EOF. Session tracking is not offered, so
// `info` always runs to the packet's end.
struct Ok {
  uint64_t affected_rows = 0;
  uint64_t last_insert_id = 0;
  uint16_t status = 0;
  uint16_t warnings = 0;
  std::string info;
  bool as_eof = false;
};
Bytes encode(const Ok& ok);

// ERR. `sqlstate` is five characters; std::invalid_argument otherwise.
struct Err {
  uint16_t code = 0;
  std::string sqlstate;
  std::string message;
};
Bytes encode(const Err& err);

Bytes encode_eof(uint16_t warnings, uint16_t status);

// The authentication exchange: more data for the client's plugin (0x01 then
// the bytes), and a request to switch plugin (0xfe, the plugin's name, its
// data).
Bytes encode_auth_more_data(std::span<const uint8_t> data);
Bytes encode_auth_switch(const std::string& plugin, std::span<const uint8_t> data);

// A text result set: the column count, one definition per column, then one
// row per packet, each value its text with a length-encoded length, or 0xfb
// for NULL.
struct ColumnDefinition {
  std::string schema;
  std::string table;
  std::string original_table;
  std::string name;
  std::string original_name;
  uint16_t character_set = 0;
  uint32_t length = 0;
  uint8_t type = 0;
  uint16_t flags = 0;
  uint8_t decimals = 0;
};
Bytes encode_column_count(uint64_t count);
Bytes encode(const ColumnDefinition& column);
Bytes encode_text_row(std::span<const std::optional<std::string>> values);

}  // namespace kvasir::protocol
