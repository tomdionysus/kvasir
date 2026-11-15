//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/protocol/messages.h"

#include <stdexcept>

namespace kvasir::protocol {

namespace {

[[noreturn]] void malformed() { throw ProtocolError(er_malformed_packet, "Malformed communication packet."); }
[[noreturn]] void bad_handshake() { throw ProtocolError(er_handshake_error, "Bad handshake"); }

// The fixed part every response begins with: flags, max packet, character
// set and 23 reserved bytes.
inline constexpr size_t response_prefix = 32;

}  // namespace

Bytes encode(const Handshake& handshake) {
  const bool plugin_auth = (handshake.capabilities & capability::plugin_auth) != 0;
  Writer w;
  w.u8(10);
  w.nul_string(handshake.server_version);
  w.u32(handshake.connection_id);
  w.raw(std::span<const uint8_t>(handshake.nonce.data(), 8));
  w.u8(0);
  w.u16(static_cast<uint16_t>(handshake.capabilities));
  w.u8(handshake.character_set);
  w.u16(handshake.status);
  w.u16(static_cast<uint16_t>(handshake.capabilities >> 16));
  // The auth data's length counts the NUL that ends its second part.
  w.u8(plugin_auth ? static_cast<uint8_t>(nonce_size + 1) : uint8_t{0});
  w.zeros(10);
  w.raw(std::span<const uint8_t>(handshake.nonce.data() + 8, nonce_size - 8));
  w.u8(0);
  if (plugin_auth) w.nul_string(handshake.auth_plugin);
  return w.take();
}

HandshakeResponse decode_handshake_response(std::span<const uint8_t> payload, uint32_t server_capabilities) {
  Reader r(payload);
  HandshakeResponse out;
  out.capabilities = r.u32();
  if (!(out.capabilities & capability::protocol_41)) bad_handshake();
  out.max_packet = r.u32();
  out.character_set = r.u8();
  r.skip(23);
  // A capability holds only when both sides name it.
  const uint32_t agreed = out.capabilities & server_capabilities;

  if ((agreed & capability::ssl) && payload.size() == response_prefix) {
    out.ssl_request = true;
    return out;
  }
  if (!(out.capabilities & capability::secure_connection)) bad_handshake();

  out.user = r.nul_string(max_user_bytes);
  if (agreed & capability::plugin_auth_lenenc_client_data) {
    const uint64_t length = r.lenenc();
    if (length > max_auth_response_bytes) malformed();
    const auto bytes = r.view(static_cast<size_t>(length));
    out.auth_response.assign(bytes.begin(), bytes.end());
  } else {
    const auto bytes = r.view(r.u8());
    out.auth_response.assign(bytes.begin(), bytes.end());
  }
  if (agreed & capability::connect_with_db) out.database = r.nul_string(max_database_bytes);
  if (agreed & capability::plugin_auth) out.auth_plugin = r.nul_string(max_plugin_bytes);
  if (agreed & capability::connect_attrs) {
    const uint64_t total = r.lenenc();
    if (total > max_attributes_bytes) malformed();
    Reader attributes(r.view(static_cast<size_t>(total)));
    while (attributes.remaining() > 0) {
      std::string key = attributes.lenenc_string(max_attributes_bytes);
      std::string value = attributes.lenenc_string(max_attributes_bytes);
      out.attributes.emplace_back(std::move(key), std::move(value));
    }
  }
  // Nothing Kvasir offers puts a field after these.
  if (r.remaining() != 0) malformed();
  return out;
}

Bytes encode(const Ok& ok) {
  Writer w;
  w.u8(ok.as_eof ? 0xfe : 0x00);
  w.lenenc(ok.affected_rows);
  w.lenenc(ok.last_insert_id);
  w.u16(ok.status);
  w.u16(ok.warnings);
  w.raw(std::string_view(ok.info));
  return w.take();
}

Bytes encode(const Err& err) {
  if (err.sqlstate.size() != 5) throw std::invalid_argument("an SQLSTATE is five characters");
  Writer w;
  w.u8(0xff);
  w.u16(err.code);
  w.u8('#');
  w.raw(std::string_view(err.sqlstate));
  w.raw(std::string_view(err.message));
  return w.take();
}

Bytes encode_eof(uint16_t warnings, uint16_t status) {
  Writer w;
  w.u8(0xfe);
  w.u16(warnings);
  w.u16(status);
  return w.take();
}

Bytes encode_auth_more_data(std::span<const uint8_t> data) {
  Writer w;
  w.u8(0x01);
  w.raw(data);
  return w.take();
}

Bytes encode_auth_switch(const std::string& plugin, std::span<const uint8_t> data) {
  Writer w;
  w.u8(0xfe);
  w.nul_string(plugin);
  w.raw(data);
  return w.take();
}

Bytes encode_column_count(uint64_t count) {
  Writer w;
  w.lenenc(count);
  return w.take();
}

Bytes encode(const ColumnDefinition& column) {
  Writer w;
  w.lenenc_string("def");
  w.lenenc_string(column.schema);
  w.lenenc_string(column.table);
  w.lenenc_string(column.original_table);
  w.lenenc_string(column.name);
  w.lenenc_string(column.original_name);
  // The length of the fixed fields that follow.
  w.lenenc(0x0c);
  w.u16(column.character_set);
  w.u32(column.length);
  w.u8(column.type);
  w.u16(column.flags);
  w.u8(column.decimals);
  w.u16(0);
  return w.take();
}

Bytes encode_text_row(std::span<const std::optional<std::string>> values) {
  Writer w;
  for (const auto& value : values) {
    if (value) {
      w.lenenc_string(*value);
    } else {
      w.u8(0xfb);
    }
  }
  return w.take();
}

}  // namespace kvasir::protocol
