//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The SQL lexer over arbitrary text, under each sql_mode it reads: every token
// lies within the text and after the one before it, no token is longer than
// the text, and the only refusal is a SyntaxError pointing into the text.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "kvasir/sql/lexer.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) return 0;
  kvasir::sql::LexerOptions options;
  options.ansi_quotes = (data[0] & 1) != 0;
  options.no_backslash_escapes = (data[0] & 2) != 0;
  const std::string_view text(reinterpret_cast<const char*>(data + 1), size - 1);
  kvasir::sql::Lexer lexer(text, options);
  size_t end = 0;
  size_t tokens = 0;
  try {
    for (;;) {
      const auto token = lexer.next();
      if (token.kind == kvasir::sql::TokenKind::end) break;
      if (token.offset < end || token.offset + token.length > text.size() || token.length == 0) std::abort();
      if (token.value.size() > text.size()) std::abort();
      end = token.offset + token.length;
      if (++tokens > text.size()) std::abort();
    }
  } catch (const kvasir::sql::SyntaxError& error) {
    if (error.offset() > text.size()) std::abort();
  }
  return 0;
}
