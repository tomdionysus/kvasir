//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The SQL parser over arbitrary text, under each sql_mode it reads and a
// small nesting bound: it returns statements that lie within the text, in
// order, or refuses with a SyntaxError pointing into the text or EmptyQuery,
// never anything else; every tree it builds prints.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "kvasir/sql/parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) return 0;
  kvasir::sql::ParserOptions options;
  options.lexer.ansi_quotes = (data[0] & 1) != 0;
  options.lexer.no_backslash_escapes = (data[0] & 2) != 0;
  options.pipes_as_concat = (data[0] & 4) != 0;
  options.max_depth = 32;
  const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
  try {
    size_t end = 0;
    for (const auto& statement : kvasir::sql::parse(text, options)) {
      if (statement.offset < end || statement.offset + statement.length > text.size()) std::abort();
      end = statement.offset + statement.length;
      for (const auto& item : statement.select.items)
        if (item.expr) (void)kvasir::sql::to_string(*item.expr);
      if (statement.select.where) (void)kvasir::sql::to_string(*statement.select.where);
      for (const auto& assignment : statement.assignments)
        if (assignment.value) (void)kvasir::sql::to_string(*assignment.value);
    }
  } catch (const kvasir::sql::SyntaxError& error) {
    if (error.offset() > text.size()) std::abort();
  } catch (const kvasir::sql::EmptyQuery&) {
  }
  return 0;
}
