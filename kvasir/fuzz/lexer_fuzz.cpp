//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The SQL lexer over arbitrary text, under each sql_mode it reads: every token
// lies within the text and after the one before it, no token is longer than
// the text, and the only refusal is a SyntaxError pointing into the text. The
// first byte picks the modes. Registered with Dannan's fuzz harness.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "kvasir/sql/lexer.h"

namespace {

void run(std::span<const uint8_t> data) {
  if (data.empty()) return;
  kvasir::sql::LexerOptions options;
  options.ansi_quotes = (data[0] & 1) != 0;
  options.no_backslash_escapes = (data[0] & 2) != 0;
  const std::string_view text(reinterpret_cast<const char*>(data.data() + 1), data.size() - 1);
  kvasir::sql::Lexer lexer(text, options);
  size_t end = 0;
  size_t tokens = 0;
  try {
    for (;;) {
      const auto token = lexer.next();
      if (token.kind == kvasir::sql::TokenKind::end) break;
      if (token.offset < end || token.offset + token.length > text.size() || token.length == 0) throw std::logic_error("a token lies outside the text");
      if (token.value.size() > text.size()) throw std::logic_error("a token is longer than the text");
      end = token.offset + token.length;
      if (++tokens > text.size()) throw std::logic_error("more tokens than bytes");
    }
  } catch (const kvasir::sql::SyntaxError& error) {
    if (error.offset() > text.size()) throw std::logic_error("a syntax error points past the text");
  }
}

dannan::net::Bytes seed(uint8_t modes, std::string_view text) {
  dannan::net::Bytes out{modes};
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

const dannan::fuzz::Registration registration("lexer", run, [] {
  return std::vector<dannan::net::Bytes>{
      seed(0, "SELECT 'a\\'b', `c d`, 1.5e3, 0x1F, b'101', X'4A', @v, @@session.sql_mode -- note\n/* c */ ;"),
      seed(1, "SELECT \"quoted\", N'n', _utf8mb4'u' COLLATE utf8mb4_bin"),
      seed(2, "SELECT 'no\\escape', /*+ SET_VAR(ack_level='ephemeral') */ 1"),
  };
});

}  // namespace
