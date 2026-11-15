//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

// The SQL lexer: statement text into tokens, as the MySQL 8.4 manual's
// "Language Structure" chapter has them. Written by hand. Text arrives from
// clients, so it is a parser of untrusted input: it reads each byte once,
// keeps nothing but the token it is building (never longer than the text),
// and refuses what it does not understand with MySQL's syntax error.
//
// Words are not classified: keywords and identifiers are both `word`, as
// written, and the parser decides which is which (MySQL's reserved words
// depend on where they stand). Adjacent strings, and a character set
// introducer before a string (`_utf8mb4'x'`), are the parser's to join.
namespace kvasir::sql {

enum class TokenKind : uint8_t {
  end,                // the end of the text
  word,               // an unquoted identifier or keyword, as written
  quoted_identifier,  // `name`, or "name" under ANSI_QUOTES; `value` unescaped
  string,             // '...', or "..." without ANSI_QUOTES; escapes resolved
  national_string,    // N'...'
  integer,            // decimal digits
  decimal,            // digits with a point: 1.5, .5, 1.
  floating,           // with an exponent: 1e3, 1.5E-3
  hex,                // X'0A' or 0x0a; `value` is the bytes
  bits,               // B'0101' or 0b0101; `value` is the bytes
  user_variable,      // @name, @'name', @`name`, @"name"; `value` is the name
  placeholder,        // ?
  hint,               // /*+ ... */ after a statement's first keyword; `value` is its text
  symbol,             // an operator or punctuation: ( ) , ; . = <=> ->> @@ ...
};

struct Token {
  TokenKind kind = TokenKind::end;
  // A word, number or symbol as written; a string or quoted identifier
  // unescaped; a hex or bit literal's bytes; a variable's name; a hint's text.
  std::string value;
  // Where it stands in the text: its first byte, its length there, its line
  // (from 1).
  size_t offset = 0;
  size_t length = 0;
  uint32_t line = 1;
};

// The session's sql_mode, as far as it changes lexing, and the version an
// executable comment (/*!80000 ... */) is compared with.
struct LexerOptions {
  bool ansi_quotes = false;
  bool no_backslash_escapes = false;
  uint32_t server_version = 80400;
};

inline constexpr uint16_t er_parse_error = 1064;

// MySQL's ER_PARSE_ERROR (1064, SQLSTATE 42000) at a point in the text: "You
// have an error in your SQL syntax; check the manual that corresponds to your
// MySQL server version for the right syntax to use near '<text>' at line <n>",
// the text being the rest of the statement from that point, cut to 80 bytes
// at a character's edge.
class SyntaxError : public std::runtime_error {
 public:
  SyntaxError(std::string_view text, size_t offset, uint32_t line);
  size_t offset() const noexcept { return _offset; }
  uint32_t line() const noexcept { return _line; }

 private:
  size_t _offset;
  uint32_t _line;
};

class Lexer {
 public:
  // Reads `text`, which the caller keeps alive while the lexer is used.
  explicit Lexer(std::string_view text, LexerOptions options = {});

  // The next token; `end` once the text is spent, and again after. Throws
  // SyntaxError for an unterminated string, identifier or comment, a hex or
  // bit literal with a digit it cannot hold, bytes that are not UTF-8 outside
  // a string, or a character no token begins with.
  Token next();

 private:
  void skip_space_and_comments();
  Token word_or_number();
  Token number(size_t start);
  Token quoted(char quote, TokenKind kind, size_t start, size_t content);
  Token hex_or_bits(TokenKind kind, size_t start, size_t content);
  Token variable(size_t start);
  Token symbol(size_t start);
  Token make(TokenKind kind, std::string value, size_t start) const;
  // The length of the UTF-8 character at `at`, or 0 for bytes that are not
  // one.
  size_t utf8_length(size_t at) const noexcept;
  bool word_byte(size_t at) const noexcept;
  [[noreturn]] void fail(size_t at) const;

  std::string_view _text;
  LexerOptions _options;
  size_t _at = 0;
  uint32_t _line = 1;
  // Inside /*! ... */ whose version is met: its */ is skipped.
  bool _in_executable = false;
  size_t _executable_start = 0;
  // The last token, for the choices that hang on it: a hint follows a
  // statement's first keyword, and digits after `ident.` are a name.
  TokenKind _last_kind = TokenKind::end;
  std::string _last_word;
  size_t _last_end = 0;
  bool _dot_after_name = false;
};

}  // namespace kvasir::sql
