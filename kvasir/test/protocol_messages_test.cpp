//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual, client/server protocol:
// - Protocol::HandshakeV10: int<1> 10, string<NUL> server version, int<4>
//   thread id, string[8] auth data part 1, int<1> 0, int<2> capabilities
//   (lower), int<1> character set, int<2> status, int<2> capabilities
//   (upper), int<1> auth data length, string[10] zeros, auth data part 2 of
//   max(13, length - 8) bytes, string<NUL> auth plugin name.
// - Protocol::HandshakeResponse41: int<4> client flags, int<4> max packet,
//   int<1> character set, string[23] zeros, string<NUL> user, the auth
//   response (length-encoded with CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA, else
//   int<1> length and bytes), string<NUL> database with
//   CLIENT_CONNECT_WITH_DB, string<NUL> plugin with CLIENT_PLUGIN_AUTH, and
//   with CLIENT_CONNECT_ATTRS a length-encoded total then key and value
//   strings. Protocol::SSLRequest is its first 32 bytes with CLIENT_SSL.
// - OK_Packet, ERR_Packet and EOF_Packet, with the manual's example bytes.
// - Protocol::ColumnDefinition41 and a text result set's row.
// - Protocol::AuthMoreData and Protocol::AuthSwitchRequest.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "kvasir/protocol/messages.h"

namespace kvasir::protocol {
namespace {

Bytes bytes(std::initializer_list<int> values) {
  Bytes out;
  for (int v : values) out.push_back(static_cast<uint8_t>(v));
  return out;
}

inline constexpr uint32_t server_capabilities = capability::long_password | capability::long_flag | capability::connect_with_db | capability::protocol_41 |
                                                capability::ssl | capability::transactions | capability::secure_connection | capability::plugin_auth |
                                                capability::connect_attrs | capability::plugin_auth_lenenc_client_data | capability::deprecate_eof;

struct ResponseParts {
  uint32_t flags = capability::protocol_41 | capability::secure_connection | capability::plugin_auth | capability::connect_with_db;
  std::string user = "root";
  std::string auth = std::string(32, '\x5a');
  std::string database = "test";
  std::string plugin = "caching_sha2_password";
  std::vector<std::pair<std::string, std::string>> attributes;
};

Bytes response(const ResponseParts& parts) {
  Writer w;
  w.u32(parts.flags);
  w.u32(16 * 1024 * 1024);
  w.u8(255);
  w.zeros(23);
  w.nul_string(parts.user);
  if (parts.flags & capability::plugin_auth_lenenc_client_data) {
    w.lenenc_string(parts.auth);
  } else {
    w.u8(static_cast<uint8_t>(parts.auth.size()));
    w.raw(std::string_view(parts.auth));
  }
  if (parts.flags & capability::connect_with_db) w.nul_string(parts.database);
  if (parts.flags & capability::plugin_auth) w.nul_string(parts.plugin);
  if (parts.flags & capability::connect_attrs) {
    Writer attributes;
    for (const auto& [key, value] : parts.attributes) {
      attributes.lenenc_string(key);
      attributes.lenenc_string(value);
    }
    w.lenenc(attributes.data().size());
    w.raw(attributes.data());
  }
  return w.take();
}

uint16_t code_of(const Bytes& payload, uint32_t capabilities = server_capabilities) {
  try {
    decode_handshake_response(payload, capabilities);
  } catch (const ProtocolError& e) {
    return e.code();
  }
  return 0;
}

// The greeting MySQL 8.4.13 sent on 2026-11-15 (Oracle's container on
// node-b, read from its port), payload only: Kvasir's encoder, given
// the same fields, writes the same bytes.
TEST(ProtocolMessages, TheGreetingMatchesMySqlsBytes) {
  Handshake h;
  h.server_version = "8.4.13";
  h.connection_id = 0x17;
  const uint8_t nonce[] = {0x1c, 0x45, 0x66, 0x53, 0x10, 0x27, 0x64, 0x1e, 0x54, 0x5d, 0x3d, 0x49, 0x17, 0x1e, 0x5e, 0x22, 0x52, 0x08, 0x36, 0x64};
  std::copy(std::begin(nonce), std::end(nonce), h.nonce.begin());
  h.capabilities = 0xdfffffff;
  h.character_set = 255;
  h.status = status::autocommit;
  h.auth_plugin = "caching_sha2_password";
  Bytes expected = bytes({0x0a, '8',  '.',  '4',  '.',  '1',  '3',  0x00, 0x17, 0x00, 0x00, 0x00, 0x1c, 0x45, 0x66, 0x53, 0x10, 0x27,
                          0x64, 0x1e, 0x00, 0xff, 0xff, 0xff, 0x02, 0x00, 0xff, 0xdf, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x54, 0x5d, 0x3d, 0x49, 0x17, 0x1e, 0x5e, 0x22, 0x52, 0x08, 0x36, 0x64, 0x00});
  const std::string plugin("caching_sha2_password");
  expected.insert(expected.end(), plugin.begin(), plugin.end());
  expected.push_back(0);
  EXPECT_EQ(expected.size(), 74u);
  EXPECT_EQ(encode(h), expected);
}

TEST(ProtocolMessages, TheGreetingIsHandshakeV10) {
  Handshake h;
  h.server_version = "8.4.0-kvasir";
  h.connection_id = 0x01020304;
  for (size_t i = 0; i < nonce_size; ++i) h.nonce[i] = static_cast<uint8_t>(0x21 + i);
  h.capabilities = server_capabilities;
  h.character_set = 255;
  h.status = status::autocommit;
  h.auth_plugin = "caching_sha2_password";
  const Bytes payload = encode(h);

  Reader r(payload);
  EXPECT_EQ(r.u8(), 10);
  EXPECT_EQ(r.nul_string(64), "8.4.0-kvasir");
  EXPECT_EQ(r.u32(), 0x01020304u);
  const auto part1 = r.view(8);
  EXPECT_EQ(Bytes(part1.begin(), part1.end()), Bytes(h.nonce.begin(), h.nonce.begin() + 8));
  EXPECT_EQ(r.u8(), 0);
  const uint32_t lower = r.u16();
  EXPECT_EQ(r.u8(), 255);
  EXPECT_EQ(r.u16(), status::autocommit);
  const uint32_t upper = r.u16();
  EXPECT_EQ(lower | (upper << 16), server_capabilities);
  // Twenty bytes of auth data and the NUL that ends them.
  EXPECT_EQ(r.u8(), 21);
  for (uint8_t zero : r.view(10)) EXPECT_EQ(zero, 0);
  const auto part2 = r.view(13);
  EXPECT_EQ(Bytes(part2.begin(), part2.begin() + 12), Bytes(h.nonce.begin() + 8, h.nonce.end()));
  EXPECT_EQ(part2[12], 0);
  EXPECT_EQ(r.nul_string(64), "caching_sha2_password");
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(ProtocolMessages, AResponseIsReadFieldByField) {
  const ResponseParts parts;
  const auto got = decode_handshake_response(response(parts), server_capabilities);
  EXPECT_FALSE(got.ssl_request);
  EXPECT_EQ(got.capabilities, parts.flags);
  EXPECT_EQ(got.max_packet, 16u * 1024 * 1024);
  EXPECT_EQ(got.character_set, 255);
  EXPECT_EQ(got.user, "root");
  EXPECT_EQ(got.auth_response, Bytes(32, 0x5a));
  ASSERT_TRUE(got.database);
  EXPECT_EQ(*got.database, "test");
  ASSERT_TRUE(got.auth_plugin);
  EXPECT_EQ(*got.auth_plugin, "caching_sha2_password");
  EXPECT_TRUE(got.attributes.empty());
}

TEST(ProtocolMessages, AnEmptyAuthResponseIsRead) {
  ResponseParts parts;
  parts.auth.clear();
  const auto got = decode_handshake_response(response(parts), server_capabilities);
  EXPECT_TRUE(got.auth_response.empty());
  EXPECT_EQ(got.user, "root");
}

TEST(ProtocolMessages, ALengthEncodedAuthResponseIsRead) {
  ResponseParts parts;
  parts.flags |= capability::plugin_auth_lenenc_client_data;
  parts.auth = std::string(300, '\x11');
  const auto got = decode_handshake_response(response(parts), server_capabilities);
  EXPECT_EQ(got.auth_response, Bytes(300, 0x11));
  ASSERT_TRUE(got.auth_plugin);
}

TEST(ProtocolMessages, AFieldIsReadOnlyWhenTheClientNamesItsCapability) {
  ResponseParts parts;
  parts.flags &= ~capability::connect_with_db;
  const auto got = decode_handshake_response(response(parts), server_capabilities);
  EXPECT_FALSE(got.database);
  ASSERT_TRUE(got.auth_plugin);
  EXPECT_EQ(*got.auth_plugin, "caching_sha2_password");

  parts.flags &= ~capability::plugin_auth;
  const auto bare = decode_handshake_response(response(parts), server_capabilities);
  EXPECT_FALSE(bare.database);
  EXPECT_FALSE(bare.auth_plugin);
}

TEST(ProtocolMessages, ConnectionAttributesAreReadInOrder) {
  ResponseParts parts;
  parts.flags |= capability::connect_attrs;
  parts.attributes = {{"_client_name", "libmysql"}, {"_pid", "4242"}, {"program_name", "mysql"}};
  const auto got = decode_handshake_response(response(parts), server_capabilities);
  EXPECT_EQ(got.attributes, parts.attributes);
}

TEST(ProtocolMessages, AnSslRequestIsTheFirst32BytesWithTheSslFlag) {
  Writer w;
  w.u32(capability::protocol_41 | capability::secure_connection | capability::ssl | capability::plugin_auth);
  w.u32(16 * 1024 * 1024);
  w.u8(255);
  w.zeros(23);
  const Bytes payload = w.take();
  ASSERT_EQ(payload.size(), 32u);
  const auto got = decode_handshake_response(payload, server_capabilities);
  EXPECT_TRUE(got.ssl_request);
  EXPECT_TRUE(got.user.empty());
  EXPECT_EQ(got.character_set, 255);

  // A server that did not offer TLS does not take it as one.
  EXPECT_EQ(code_of(payload, server_capabilities & ~capability::ssl), er_malformed_packet);
}

TEST(ProtocolMessages, AClientWithoutProtocol41OrSecureAuthenticationIsABadHandshake) {
  ResponseParts parts;
  parts.flags &= ~capability::protocol_41;
  EXPECT_EQ(code_of(response(parts)), er_handshake_error);

  ResponseParts old_auth;
  old_auth.flags &= ~capability::secure_connection;
  EXPECT_EQ(code_of(response(old_auth)), er_handshake_error);
}

TEST(ProtocolMessages, EveryTruncationOfAResponseIsRefused) {
  ResponseParts parts;
  parts.flags |= capability::connect_attrs;
  parts.attributes = {{"_client_name", "libmysql"}};
  const Bytes whole = response(parts);
  ASSERT_EQ(code_of(whole), 0);
  for (size_t size = 0; size < whole.size(); ++size) {
    const Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(size));
    EXPECT_NE(code_of(cut), 0) << "a response cut to " << size << " bytes was accepted";
  }
}

TEST(ProtocolMessages, BytesAfterTheLastFieldAreRefused) {
  Bytes payload = response(ResponseParts{});
  payload.push_back(0x00);
  EXPECT_EQ(code_of(payload), er_malformed_packet);
}

TEST(ProtocolMessages, FieldsOverTheirBoundsAreRefused) {
  ResponseParts long_user;
  long_user.user = std::string(max_user_bytes + 1, 'u');
  EXPECT_EQ(code_of(response(long_user)), er_malformed_packet);

  ResponseParts at_bound;
  at_bound.user = std::string(max_user_bytes, 'u');
  EXPECT_EQ(code_of(response(at_bound)), 0);

  ResponseParts long_auth;
  long_auth.flags |= capability::plugin_auth_lenenc_client_data;
  long_auth.auth = std::string(max_auth_response_bytes + 1, 'a');
  EXPECT_EQ(code_of(response(long_auth)), er_malformed_packet);

  ResponseParts long_database;
  long_database.database = std::string(max_database_bytes + 1, 'd');
  EXPECT_EQ(code_of(response(long_database)), er_malformed_packet);

  ResponseParts long_plugin;
  long_plugin.plugin = std::string(max_plugin_bytes + 1, 'p');
  EXPECT_EQ(code_of(response(long_plugin)), er_malformed_packet);

  ResponseParts many_attributes;
  many_attributes.flags |= capability::connect_attrs;
  many_attributes.attributes = {{"k", std::string(max_attributes_bytes, 'v')}};
  EXPECT_EQ(code_of(response(many_attributes)), er_malformed_packet);
}

TEST(ProtocolMessages, AttributesThatDoNotFillTheirStatedLengthAreRefused) {
  ResponseParts parts;
  Writer w;
  w.raw(response(parts));
  // Stated: 10 bytes of attributes. Sent: a key of 3 and nothing after.
  Bytes payload = w.take();
  // Turn the attributes capability on in the flags already written.
  const uint32_t flags = parts.flags | capability::connect_attrs;
  payload[0] = static_cast<uint8_t>(flags);
  payload[1] = static_cast<uint8_t>(flags >> 8);
  payload[2] = static_cast<uint8_t>(flags >> 16);
  payload[3] = static_cast<uint8_t>(flags >> 24);
  const Bytes attributes = bytes({10, 3, 'k', 'e', 'y'});
  payload.insert(payload.end(), attributes.begin(), attributes.end());
  EXPECT_EQ(code_of(payload), er_malformed_packet);
}

TEST(ProtocolMessages, OkIsTheManualsExample) {
  Ok ok;
  ok.status = status::autocommit;
  EXPECT_EQ(encode(ok), bytes({0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00}));
}

TEST(ProtocolMessages, OkCarriesItsCountsAndItsInfoToTheEnd) {
  Ok ok;
  ok.affected_rows = 300;
  ok.last_insert_id = 7;
  ok.status = status::autocommit | status::in_transaction;
  ok.warnings = 2;
  ok.info = "Rows matched: 300";
  Bytes expected = bytes({0x00, 0xfc, 0x2c, 0x01, 0x07, 0x03, 0x00, 0x02, 0x00});
  for (char c : ok.info) expected.push_back(static_cast<uint8_t>(c));
  EXPECT_EQ(encode(ok), expected);
}

TEST(ProtocolMessages, OkThatEndsAResultSetHasTheEofHeader) {
  Ok ok;
  ok.status = status::autocommit;
  ok.as_eof = true;
  EXPECT_EQ(encode(ok), bytes({0xfe, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00}));
}

TEST(ProtocolMessages, ErrIsTheManualsExample) {
  const Err err{1096, "HY000", "No tables used"};
  EXPECT_EQ(encode(err),
            bytes({0xff, 0x48, 0x04, 0x23, 0x48, 0x59, 0x30, 0x30, 0x30, 0x4e, 0x6f, 0x20, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x73, 0x20, 0x75, 0x73, 0x65, 0x64}));
}

TEST(ProtocolMessages, ErrRefusesASqlstateThatIsNotFiveCharacters) {
  EXPECT_THROW(encode(Err{1096, "HY00", "No tables used"}), std::invalid_argument);
  EXPECT_THROW(encode(Err{1096, "HY0000", "No tables used"}), std::invalid_argument);
}

TEST(ProtocolMessages, EofIsTheManualsExample) { EXPECT_EQ(encode_eof(0, status::autocommit), bytes({0xfe, 0x00, 0x00, 0x02, 0x00})); }

TEST(ProtocolMessages, AuthMoreDataAndAuthSwitch) {
  // caching_sha2_password's "fast authentication succeeded" is 0x03.
  EXPECT_EQ(encode_auth_more_data(bytes({0x03})), bytes({0x01, 0x03}));
  EXPECT_EQ(encode_auth_switch("abc", bytes({0x10, 0x20})), bytes({0xfe, 'a', 'b', 'c', 0x00, 0x10, 0x20}));
}

TEST(ProtocolMessages, AColumnDefinitionIsColumnDefinition41) {
  ColumnDefinition column;
  column.schema = "s";
  column.table = "t";
  column.original_table = "ot";
  column.name = "@@version_comment";
  column.original_name = "";
  column.character_set = 255;
  column.length = 112;
  column.type = 0xfd;
  column.flags = 0x0001;
  column.decimals = 31;
  const Bytes payload = encode(column);

  Reader r(payload);
  EXPECT_EQ(r.lenenc_string(8), "def");
  EXPECT_EQ(r.lenenc_string(8), "s");
  EXPECT_EQ(r.lenenc_string(8), "t");
  EXPECT_EQ(r.lenenc_string(8), "ot");
  EXPECT_EQ(r.lenenc_string(32), "@@version_comment");
  EXPECT_EQ(r.lenenc_string(8), "");
  EXPECT_EQ(r.lenenc(), 0x0cu);
  EXPECT_EQ(r.u16(), 255);
  EXPECT_EQ(r.u32(), 112u);
  EXPECT_EQ(r.u8(), 0xfd);
  EXPECT_EQ(r.u16(), 0x0001);
  EXPECT_EQ(r.u8(), 31);
  EXPECT_EQ(r.u16(), 0);
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(ProtocolMessages, ATextRowIsItsValuesWithNullAs0xfb) {
  EXPECT_EQ(encode_column_count(1), bytes({0x01}));
  EXPECT_EQ(encode_column_count(300), bytes({0xfc, 0x2c, 0x01}));
  const std::vector<std::optional<std::string>> row = {std::string("1"), std::nullopt, std::string(""), std::string("ab")};
  EXPECT_EQ(encode_text_row(row), bytes({0x01, '1', 0xfb, 0x00, 0x02, 'a', 'b'}));
}

}  // namespace
}  // namespace kvasir::protocol
