//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "kvasir/protocol/codec.h"

// Prepared statements' messages, the MySQL 8.4 manual's "Prepared
// Statements" in the client/server protocol, as MySQL 8.4.13 sends and
// reads them (captured): the reply to COM_STMT_PREPARE, the body of
// COM_STMT_EXECUTE, and the binary protocol's rows. Kvasir offers neither
// CLIENT_QUERY_ATTRIBUTES nor optional result-set metadata, so their forms
// never arrive. The execute reader is a parser of untrusted input: bounded
// by the statement's parameter count and by what is left of the packet.
namespace kvasir::protocol {

// COM_STMT_PREPARE's reply: 0x00, the statement's id, how many columns and
// parameters it has, a reserved 0x00, its warning count. The parameters'
// definitions and the columns' follow, each list ended as a result set's.
struct PrepareOk {
  uint32_t statement = 0;
  uint16_t columns = 0;
  uint16_t params = 0;
  uint16_t warnings = 0;
};
Bytes encode(const PrepareOk& ok);

// A binary value's type, as a parameter's is sent and a column's is
// written: the field type, and whether an integer is unsigned.
struct ValueType {
  uint8_t type = 0;
  bool is_unsigned = false;
};

// A bound parameter. Empty for NULL; a TINY, SHORT, LONG, INT24, LONGLONG
// or YEAR is an integer by its signedness; a FLOAT or DOUBLE a double; any
// other type its bytes (a date or time's as sent, after their length byte).
struct BoundValue {
  ValueType type;
  std::variant<std::monostate, int64_t, uint64_t, double, std::string> value;
};

// COM_STMT_EXECUTE's body, after its command byte.
struct Execute {
  uint32_t statement = 0;
  // The cursor flags: 0, no cursor.
  uint8_t flags = 0;
  uint32_t iterations = 0;
  std::vector<BoundValue> params;
};

// The statement id the body of COM_STMT_EXECUTE, SEND_LONG_DATA, CLOSE or
// RESET begins with. Throws ProtocolError (er_malformed_packet) for fewer
// than four bytes.
uint32_t statement_of(std::span<const uint8_t> body);

// Reads an execute for a statement of `bound.size()` parameters. `bound`
// holds the types the client bound last; a body that binds new ones
// replaces them, one that does not uses them (refused when none were ever
// bound). A parameter with `long_data` (COM_STMT_SEND_LONG_DATA) takes it,
// and its value is not in the body. Throws ProtocolError
// (er_malformed_packet) for bytes that do not fit, a type the protocol does
// not have, or a value longer than what is left.
Execute decode_execute(std::span<const uint8_t> body, std::vector<std::optional<ValueType>>& bound, std::span<const std::optional<std::string>> long_data);

// One binary row: 0x00, a NULL bitmap whose bits start at 2, then each
// value not NULL by its column's type, from the text a text row would
// carry: TINY 1 byte, SHORT and YEAR 2, LONG and INT24 4, LONGLONG 8, FLOAT
// 4 and DOUBLE 8 (IEEE, little-endian), anything else length-encoded.
// Throws std::invalid_argument for a column count that does not match, or
// a number's text that is not one.
Bytes encode_binary_row(std::span<const ValueType> columns, std::span<const std::optional<std::string>> values);

}  // namespace kvasir::protocol
