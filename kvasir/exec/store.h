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
    too_long,      // longer than the column
  };
  Result result = Result::stored;
  // The value as stored, in the text a client is sent; for stored and
  // truncated only.
  std::string text;
};

// `from` is the type of the expression `value` came from (a hex literal is a
// number in a numeric column, bytes in a string one).
Stored store(const catalog::ColumnType& type, const Value& value, const ResultType& from);

}  // namespace kvasir::exec
