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

// UPPER() and LOWER()'s case mapping, as MySQL 8.4.13 maps every code point
// (checked against the reference server, test/differential/casing.tsv):
// Unicode's simple mappings, one character to one, from Unicode 9.0.0 under
// utf8mb4's 0900 collations, and from Unicode 3.0.0 in the BMP only under
// every other collation (utf8mb4_bin and _general_ci, the utf8mb3 ones); a
// binary string is never changed. MySQL maps in place, in a buffer the
// argument's length, so a mapping that lengthens a character can end the
// result early (observed; upper()'s comment). The tables are compiled from
// kvasir/data/unicode/ by kvasir-casing at build time.
namespace kvasir::exec {

enum class Casing : uint8_t { none, unicode3, unicode9 };

// The mapping a collation's UPPER() and LOWER() use.
Casing casing_of(uint16_t collation);

// The string with each character mapped, in place in a buffer of its
// length: the result ends at the first character that no longer fits, or
// at bytes that are not (or, once written over, no longer are) a character.
std::string upper(std::string_view utf8, Casing casing);
std::string lower(std::string_view utf8, Casing casing);

}  // namespace kvasir::exec
