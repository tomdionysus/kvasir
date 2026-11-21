//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The SQL parser over arbitrary text, under each sql_mode it reads and a
// small nesting bound: it returns statements that lie within the text, in
// order, or refuses with a SyntaxError pointing into the text or EmptyQuery,
// never anything else; every tree it builds prints. The first byte picks the
// modes. Registered with Dannan's fuzz harness (dannan/fuzz/fuzz.h).

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "kvasir/sql/parser.h"

namespace {

void run(std::span<const uint8_t> data) {
  if (data.empty()) return;
  kvasir::sql::ParserOptions options;
  options.lexer.ansi_quotes = (data[0] & 1) != 0;
  options.lexer.no_backslash_escapes = (data[0] & 2) != 0;
  options.pipes_as_concat = (data[0] & 4) != 0;
  options.max_depth = 32;
  const std::string_view text(reinterpret_cast<const char*>(data.data() + 1), data.size() - 1);
  try {
    size_t end = 0;
    for (const auto& statement : kvasir::sql::parse(text, options)) {
      if (statement.offset < end || statement.offset + statement.length > text.size()) throw std::logic_error("a statement lies outside the text");
      end = statement.offset + statement.length;
      for (const auto& item : statement.select.items)
        if (item.expr) (void)kvasir::sql::to_string(*item.expr);
      if (statement.select.where) (void)kvasir::sql::to_string(*statement.select.where);
      for (const auto& assignment : statement.assignments)
        if (assignment.value) (void)kvasir::sql::to_string(*assignment.value);
    }
  } catch (const kvasir::sql::SyntaxError& error) {
    if (error.offset() > text.size()) throw std::logic_error("a syntax error points past the text");
  } catch (const kvasir::sql::EmptyQuery&) {
  }
}

dannan::net::Bytes seed(uint8_t modes, std::string_view text) {
  dannan::net::Bytes out{modes};
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

const dannan::fuzz::Registration registration("parser", run, [] {
  return std::vector<dannan::net::Bytes>{
      seed(0, "SELECT a, COUNT(*) FROM t WHERE b = 1 GROUP BY a HAVING COUNT(*) > 1 ORDER BY 1 DESC LIMIT 2, 3"),
      seed(0, "INSERT INTO t (a, b) VALUES (1, 'x'), (2, NULL); UPDATE t SET b = CONCAT(b, 'y') WHERE a IN (1, 2)"),
      seed(1, "SELECT \"a\" FROM `t` WHERE c LIKE 'x%' ESCAPE '|'"),
      seed(4, "SELECT 'a' || 'b', CASE WHEN 1 THEN 2 ELSE 3 END, -(1 + 2) * 3 DIV 4"),
      seed(0, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(10) NOT NULL DEFAULT 'x')"),
  };
});

}  // namespace
