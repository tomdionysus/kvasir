//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Prepared statements' messages: the MySQL 8.4 manual's COM_STMT_PREPARE
// response, COM_STMT_EXECUTE and "Binary Protocol Resultset Row", and the
// bytes MySQL 8.4.13 exchanged with Go's driver (go-sql-driver/mysql 1.8.1)
// and mysql-connector-python 26.10, captured through a proxy on 2026-11-20.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kvasir/protocol/statements.h"
#include "kvasir/protocol/types.h"

namespace kvasir::protocol {
namespace {

namespace f = field_type;

Bytes hex(std::string_view text) {
  Bytes out;
  std::string digits;
  for (const char ch : text)
    if (ch != ' ') digits.push_back(ch);
  for (size_t i = 0; i + 1 < digits.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(digits.substr(i, 2), nullptr, 16)));
  return out;
}

using Bound = std::vector<std::optional<ValueType>>;
const std::vector<std::optional<std::string>> no_long_data(8);

uint16_t refusal(const Bytes& body, Bound bound) {
  try {
    decode_execute(body, bound, std::span(no_long_data).first(bound.size()));
  } catch (const ProtocolError& e) {
    return e.code();
  }
  return 0;
}

TEST(ProtocolStatements, ThePrepareReply) {
  // SELECT a, b FROM t WHERE a > ? AND b <> ?: statement 1, two columns, two
  // parameters.
  EXPECT_EQ(encode(PrepareOk{1, 2, 2, 0}), hex("00 01000000 0200 0200 00 0000"));
  // INSERT INTO t VALUES (?, ?): no columns.
  EXPECT_EQ(encode(PrepareOk{4, 0, 2, 0}), hex("00 04000000 0000 0200 00 0000"));
  EXPECT_EQ(encode(PrepareOk{0x01020304, 0, 0, 3}), hex("00 04030201 0000 0000 00 0300"));
}

TEST(ProtocolStatements, AnExecuteBindsItsTypes) {
  // Go: a > 0 AND b <> 'zz'.
  Bound bound(2);
  auto e = decode_execute(hex("01000000 00 01000000 00 01 0800 fe00 0000000000000000 027a7a"), bound, std::span(no_long_data).first(2));
  EXPECT_EQ(e.statement, 1u);
  EXPECT_EQ(e.flags, 0);
  EXPECT_EQ(e.iterations, 1u);
  ASSERT_EQ(e.params.size(), 2u);
  EXPECT_EQ(e.params[0].type.type, f::longlong);
  EXPECT_FALSE(e.params[0].type.is_unsigned);
  EXPECT_EQ(std::get<int64_t>(e.params[0].value), 0);
  EXPECT_EQ(e.params[1].type.type, f::string);
  EXPECT_EQ(std::get<std::string>(e.params[1].value), "zz");
  ASSERT_TRUE(bound[0] && bound[1]);
  EXPECT_EQ(bound[1]->type, f::string);

  // The same statement again without new types: the last ones hold.
  e = decode_execute(hex("01000000 00 01000000 00 00 0500000000000000 027879"), bound, std::span(no_long_data).first(2));
  EXPECT_EQ(std::get<int64_t>(e.params[0].value), 5);
  EXPECT_EQ(std::get<std::string>(e.params[1].value), "xy");
}

TEST(ProtocolStatements, EachTypeOfValue) {
  // Go: -5, 2.5, "ab", NULL (bit 3 of the bitmap).
  Bound bound(4);
  auto e =
      decode_execute(hex("02000000 00 01000000 08 01 0800 0500 fe00 0600 fbffffffffffffff 0000000000000440 026162"), bound, std::span(no_long_data).first(4));
  EXPECT_EQ(std::get<int64_t>(e.params[0].value), -5);
  EXPECT_EQ(std::get<double>(e.params[1].value), 2.5);
  EXPECT_EQ(std::get<std::string>(e.params[2].value), "ab");
  EXPECT_TRUE(std::holds_alternative<std::monostate>(e.params[3].value));
  EXPECT_EQ(e.params[3].type.type, f::null);

  // Go: the largest BIGINT UNSIGNED, 1.5, a TINY true.
  Bound three(3);
  e = decode_execute(hex("01000000 00 01000000 00 01 0880 0500 0100 ffffffffffffffff 000000000000f83f 01"), three, std::span(no_long_data).first(3));
  EXPECT_TRUE(e.params[0].type.is_unsigned);
  EXPECT_EQ(std::get<uint64_t>(e.params[0].value), UINT64_MAX);
  EXPECT_EQ(std::get<double>(e.params[1].value), 1.5);
  EXPECT_EQ(e.params[2].type.type, f::tiny);
  EXPECT_EQ(std::get<int64_t>(e.params[2].value), 1);

  // A TINY, SHORT, LONG and INT24 are signed unless flagged; FLOAT is four
  // bytes, YEAR two (the manual's Binary Protocol Value).
  Bound six(6);
  e = decode_execute(hex("01000000 00 01000000 00 01 0100 0200 0300 0900 0400 0d00 ff feff fdffffff fcffffff 0000c03f e807"), six,
                     std::span(no_long_data).first(6));
  EXPECT_EQ(std::get<int64_t>(e.params[0].value), -1);
  EXPECT_EQ(std::get<int64_t>(e.params[1].value), -2);
  EXPECT_EQ(std::get<int64_t>(e.params[2].value), -3);
  EXPECT_EQ(std::get<int64_t>(e.params[3].value), -4);
  EXPECT_EQ(std::get<double>(e.params[4].value), 1.5);
  EXPECT_EQ(std::get<int64_t>(e.params[5].value), 2024);
  Bound unsigned_tiny(1);
  e = decode_execute(hex("01000000 00 01000000 00 01 0180 ff"), unsigned_tiny, std::span(no_long_data).first(1));
  EXPECT_EQ(std::get<uint64_t>(e.params[0].value), 255u);

  // python: a DECIMAL as its text.
  Bound one(1);
  e = decode_execute(hex("01000000 00 01000000 00 01 0000 04312e3235"), one, std::span(no_long_data).first(1));
  EXPECT_EQ(e.params[0].type.type, f::decimal);
  EXPECT_EQ(std::get<std::string>(e.params[0].value), "1.25");

  // A date's bytes as sent, after their length (the manual's MYSQL_TYPE_DATE).
  Bound date(1);
  e = decode_execute(hex("01000000 00 01000000 00 01 0a00 04e8070b14"), date, std::span(no_long_data).first(1));
  EXPECT_EQ(std::get<std::string>(e.params[0].value), std::string("\xe8\x07\x0b\x14", 4));

  // No parameters: the body ends after the iteration count.
  Bound none;
  e = decode_execute(hex("07000000 00 01000000"), none, {});
  EXPECT_EQ(e.statement, 7u);
  EXPECT_TRUE(e.params.empty());
}

TEST(ProtocolStatements, LongDataIsNotInTheBody) {
  Bound bound(2);
  const std::vector<std::optional<std::string>> long_data{std::string("sent before"), std::nullopt};
  const auto e = decode_execute(hex("01000000 00 01000000 00 01 fe00 0800 0700000000000000"), bound, long_data);
  EXPECT_EQ(std::get<std::string>(e.params[0].value), "sent before");
  EXPECT_EQ(std::get<int64_t>(e.params[1].value), 7);
}

TEST(ProtocolStatements, WhatDoesNotFitIsRefused) {
  // Too short for its header, its bitmap, its types or its values.
  EXPECT_EQ(refusal(hex("010000"), Bound(0)), er_malformed_packet);
  EXPECT_EQ(refusal(hex("01000000 00 010000"), Bound(0)), er_malformed_packet);
  EXPECT_EQ(refusal(hex("01000000 00 01000000"), Bound(1)), er_malformed_packet);
  EXPECT_EQ(refusal(hex("01000000 00 01000000 00 01 0800"), Bound(2)), er_malformed_packet);
  EXPECT_EQ(refusal(hex("01000000 00 01000000 00 01 0800 01000000"), Bound(1)), er_malformed_packet);
  EXPECT_EQ(refusal(hex("01000000 00 01000000 00 01 fe00 05616263"), Bound(1)), er_malformed_packet);
  // No types bound before, and none now.
  EXPECT_EQ(refusal(hex("01000000 00 01000000 00 00 0100000000000000"), Bound(1)), er_malformed_packet);
  // A type the protocol does not have.
  EXPECT_EQ(refusal(hex("01000000 00 01000000 00 01 7f00 00"), Bound(1)), er_malformed_packet);
  // A bound flag other than 0 or 1.
  EXPECT_EQ(refusal(hex("01000000 00 01000000 00 02 0800 0100000000000000"), Bound(1)), er_malformed_packet);
  // Bytes left over.
  EXPECT_EQ(refusal(hex("01000000 00 01000000 00 01 0800 0100000000000000 00"), Bound(1)), er_malformed_packet);
  EXPECT_EQ(statement_of(hex("2a000000ff")), 42u);
  EXPECT_THROW(statement_of(hex("2a00")), ProtocolError);
}

TEST(ProtocolStatements, BinaryRows) {
  // Go: -5, 3.5, "abx", NULL (bit 3 + 2 of the bitmap).
  const std::vector<ValueType> four{{f::longlong, false}, {f::double_, false}, {f::medium_blob, false}, {f::var_string, false}};
  EXPECT_EQ(encode_binary_row(four, std::vector<std::optional<std::string>>{"-5", "3.5", "abx", std::nullopt}),
            hex("00 20 fbffffffffffffff 0000000000000c40 03616278"));
  // A table's INT and VARCHAR.
  const std::vector<ValueType> two{{f::long_, false}, {f::var_string, false}};
  EXPECT_EQ(encode_binary_row(two, std::vector<std::optional<std::string>>{"1", "x"}), hex("00 00 01000000 0178"));
  // The largest BIGINT UNSIGNED and 1.5, then a NEWDECIMAL as text.
  const std::vector<ValueType> three{{f::longlong, true}, {f::double_, false}, {f::newdecimal, false}};
  EXPECT_EQ(encode_binary_row(three, std::vector<std::optional<std::string>>{"18446744073709551615", "1.5", "1.25"}),
            hex("00 00 ffffffffffffffff 000000000000f83f 04312e3235"));
  // The other widths; nine columns need two bitmap bytes.
  const std::vector<ValueType> nine{{f::tiny, false}, {f::short_, false}, {f::int24, false}, {f::year, false},  {f::float_, false},
                                    {f::tiny, true},  {f::null, false},   {f::long_, true},  {f::string, false}};
  EXPECT_EQ(encode_binary_row(nine, std::vector<std::optional<std::string>>{"-1", "-2", "-4", "2024", "1.5", "255", std::nullopt, "4294967295", ""}),
            hex("00 0001 ff feff fcffffff e807 0000c03f ff ffffffff 00"));
  EXPECT_THROW(encode_binary_row(two, std::vector<std::optional<std::string>>{"1"}), std::invalid_argument);
  EXPECT_THROW(encode_binary_row(two, std::vector<std::optional<std::string>>{"one", "x"}), std::invalid_argument);
}

}  // namespace
}  // namespace kvasir::protocol
