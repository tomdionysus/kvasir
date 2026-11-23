//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>

#include "kvasir/catalog/table.h"
#include "kvasir/exec/value.h"

// A value put into a column of a table, as MySQL 8.4 stores it under strict
// mode: an integer column rounds (a double to even, a decimal or a string
// half away from zero), a DECIMAL rounds to its scale with a note when digits
// are lost, a string past its length is refused unless only spaces are lost
// (silently for CHAR, with a note for the rest), and anything out of the
// type's range, or not a number where one is needed, is refused. Observed on
// MySQL 8.4.13 through CREATE TABLE's DEFAULT (2026-11-16); INSERT will give
// the same outcomes their row errors.
namespace kvasir::exec {

struct Stored {
  enum class Result : uint8_t {
    stored,        // as given, or rounded without a word
    truncated,     // stored, losing digits past the scale or trailing spaces (Note 1265)
    out_of_range,  // past the type's range
    invalid,       // not a number where one is needed, or not text of the column's character set
    garbage,       // a number with more after it (1265 for an integer or DOUBLE column, 1366 for DECIMAL)
    too_long,      // longer than the column
  };
  Result result = Result::stored;
  // The value as stored, in the text a client is sent; for stored and
  // truncated only.
  std::string text;
  // For the rest, what a statement that ignores errors (INSERT IGNORE) stores
  // in its place (observed on MySQL 8.4.13): a number past the type's range
  // its limit, a number with more after it its leading number (rounded as
  // a string rounds), anything else not a number 0, text too long cut to the
  // column, text not of the column's character set cut before its first bad
  // byte.
  std::string lenient;
};

// `from` is the type of the expression `value` came from (a hex literal is a
// number in a numeric column, bytes in a string one). A TIMESTAMP is given
// in a zone `zone_seconds` east of UTC and stored in UTC.
Stored store(const catalog::ColumnType& type, const Value& value, const ResultType& from, int32_t zone_seconds = 0);

// What a NOT NULL column without a default takes when a statement that
// ignores errors gives it NULL or nothing: 0 in the type's form, '' for
// text, zero bytes filling a BINARY.
std::string implicit_default(const catalog::ColumnType& type);

// Where the first `count` characters of UTF-8 text end: a CHAR or VARCHAR
// column's length, and a key's prefix on a text column.
size_t character_end(const std::string& s, uint64_t count);

}  // namespace kvasir::exec
