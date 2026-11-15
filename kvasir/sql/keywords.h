//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <string>
#include <string_view>

// MySQL 8.4's reserved words, those the manual's "Keywords and Reserved
// Words" marks (R): 262 of them, read from the 8.4 manual on 2026-11-15
// (QUALIFY and TABLESAMPLE new in 8.4). A reserved word is a name only when
// quoted, or after a qualifier's dot.
namespace kvasir::sql {

// Case-insensitive, ASCII.
bool is_reserved(std::string_view word) noexcept;
bool same_word(std::string_view a, std::string_view b) noexcept;
std::string upper(std::string_view word);

}  // namespace kvasir::sql
