//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>

// The numbers a column definition carries, as the MySQL client/server
// protocol defines them: its type, its flags and its collation.
namespace kvasir::protocol {

namespace field_type {
inline constexpr uint8_t decimal = 0;
inline constexpr uint8_t tiny = 1;
inline constexpr uint8_t short_ = 2;
inline constexpr uint8_t long_ = 3;
inline constexpr uint8_t float_ = 4;
inline constexpr uint8_t double_ = 5;
inline constexpr uint8_t null = 6;
inline constexpr uint8_t timestamp = 7;
inline constexpr uint8_t longlong = 8;
inline constexpr uint8_t int24 = 9;
inline constexpr uint8_t date = 10;
inline constexpr uint8_t time = 11;
inline constexpr uint8_t datetime = 12;
inline constexpr uint8_t year = 13;
inline constexpr uint8_t varchar = 15;
inline constexpr uint8_t bit = 16;
inline constexpr uint8_t json = 245;
inline constexpr uint8_t newdecimal = 246;
inline constexpr uint8_t enum_ = 247;
inline constexpr uint8_t set = 248;
inline constexpr uint8_t tiny_blob = 249;
inline constexpr uint8_t medium_blob = 250;
inline constexpr uint8_t long_blob = 251;
inline constexpr uint8_t blob = 252;
inline constexpr uint8_t var_string = 253;
inline constexpr uint8_t string = 254;
inline constexpr uint8_t geometry = 255;
}  // namespace field_type

namespace column_flag {
inline constexpr uint16_t not_null = 1;
inline constexpr uint16_t primary_key = 2;
inline constexpr uint16_t unique_key = 4;
inline constexpr uint16_t multiple_key = 8;
inline constexpr uint16_t blob = 16;
inline constexpr uint16_t unsigned_ = 32;
inline constexpr uint16_t zerofill = 64;
inline constexpr uint16_t binary = 128;
inline constexpr uint16_t enum_ = 256;
inline constexpr uint16_t auto_increment = 512;
inline constexpr uint16_t timestamp = 1024;
inline constexpr uint16_t set = 2048;
inline constexpr uint16_t no_default_value = 4096;
inline constexpr uint16_t on_update_now = 8192;
inline constexpr uint16_t part_key = 16384;
inline constexpr uint16_t num = 32768;
}  // namespace column_flag

namespace collation {
inline constexpr uint16_t binary = 63;
inline constexpr uint16_t utf8mb4_0900_ai_ci = 255;
}  // namespace collation

// A column of a type with no fixed number of decimals (a DOUBLE computed, a
// string) reports 31.
inline constexpr uint8_t not_fixed_decimals = 31;

}  // namespace kvasir::protocol
