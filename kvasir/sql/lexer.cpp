//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/sql/lexer.h"

#include <array>
#include <cctype>

#include "kvasir/sql/keywords.h"

namespace kvasir::sql {

namespace {

// The longest first, so a match is the longest.
constexpr std::array<std::string_view, 34> symbols = {
    "<=>", "->>", "<=", ">=", "<>", "!=", "<<", ">>", ":=", "&&", "||", "->", "@@", "(", ")", ",", ";",
    ".",   "=",   "<",  ">",  "+",  "-",  "*",  "/",  "%",  "^",  "&",  "|",  "~",  "!", "{", "}", ":",
};

// The keywords an optimizer hint may follow; COMMIT is Kvasir's, for
// ack_level.
constexpr std::array<std::string_view, 6> hint_keywords = {"SELECT", "INSERT", "REPLACE", "UPDATE", "DELETE", "COMMIT"};

bool ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool hex_digit(char c) noexcept { return ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
uint8_t hex_value(char c) noexcept {
  if (ascii_digit(c)) return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
  return static_cast<uint8_t>(c - 'A' + 10);
}
bool space(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// Hex digits, padded on the left to whole bytes.
std::string hex_bytes(std::string_view digits) {
  std::string out;
  size_t i = 0;
  if (digits.size() % 2) {
    out.push_back(static_cast<char>(hex_value(digits[0])));
    i = 1;
  }
  for (; i < digits.size(); i += 2) out.push_back(static_cast<char>((hex_value(digits[i]) << 4) | hex_value(digits[i + 1])));
  return out;
}

// Bits, padded on the left to whole bytes.
std::string bit_bytes(std::string_view digits) {
  std::string out;
  const size_t lead = digits.size() % 8;
  size_t i = 0;
  const auto take = [&](size_t count) {
    uint8_t byte = 0;
    for (size_t k = 0; k < count; ++k, ++i) byte = static_cast<uint8_t>((byte << 1) | (digits[i] == '1' ? 1 : 0));
    out.push_back(static_cast<char>(byte));
  };
  if (lead) take(lead);
  while (i < digits.size()) take(8);
  return out;
}

std::string message(std::string_view text, size_t offset, uint32_t line) {
  auto rest = offset < text.size() ? text.substr(offset) : std::string_view();
  if (rest.size() > 80) {
    size_t cut = 80;
    while (cut > 0 && (static_cast<uint8_t>(rest[cut]) & 0xc0) == 0x80) --cut;
    rest = rest.substr(0, cut);
  }
  return "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near '" +
         std::string(rest) + "' at line " + std::to_string(line);
}

}  // namespace

SyntaxError::SyntaxError(std::string_view text, size_t offset, uint32_t line) : std::runtime_error(message(text, offset, line)), _offset(offset), _line(line) {}

Lexer::Lexer(std::string_view text, LexerOptions options) : _text(text), _options(options) {}

void Lexer::fail(size_t at) const {
  uint32_t line = 1;
  for (size_t i = 0; i < at && i < _text.size(); ++i)
    if (_text[i] == '\n') ++line;
  throw SyntaxError(_text, at, line);
}

size_t Lexer::utf8_length(size_t at) const noexcept {
  const auto byte = [&](size_t i) { return static_cast<uint8_t>(_text[i]); };
  const uint8_t lead = byte(at);
  if (lead < 0x80) return 1;
  size_t length = 0;
  uint32_t point = 0;
  if ((lead & 0xe0) == 0xc0) {
    length = 2;
    point = lead & 0x1f;
  } else if ((lead & 0xf0) == 0xe0) {
    length = 3;
    point = lead & 0x0f;
  } else if ((lead & 0xf8) == 0xf0) {
    length = 4;
    point = lead & 0x07;
  } else {
    return 0;
  }
  if (at + length > _text.size()) return 0;
  for (size_t i = 1; i < length; ++i) {
    if ((byte(at + i) & 0xc0) != 0x80) return 0;
    point = (point << 6) | (byte(at + i) & 0x3f);
  }
  // Overlong forms, surrogates, and past U+10FFFF.
  if ((length == 2 && point < 0x80) || (length == 3 && point < 0x800) || (length == 4 && point < 0x10000)) return 0;
  if ((point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) return 0;
  return length;
}

bool Lexer::word_byte(size_t at) const noexcept {
  if (at >= _text.size()) return false;
  const char c = _text[at];
  if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') return true;
  // U+0080 to U+FFFF, as the manual permits in an unquoted name.
  const auto length = utf8_length(at);
  return length == 2 || length == 3;
}

void Lexer::skip_space_and_comments() {
  for (;;) {
    if (_at >= _text.size()) break;
    const char c = _text[_at];
    if (space(c)) {
      if (c == '\n') ++_line;
      ++_at;
      continue;
    }
    const auto rest = _text.substr(_at);
    const bool dashes = rest.size() >= 2 && rest[0] == '-' && rest[1] == '-' && (rest.size() == 2 || static_cast<uint8_t>(rest[2]) <= ' ');
    if (c == '#' || dashes) {
      while (_at < _text.size() && _text[_at] != '\n') ++_at;
      continue;
    }
    if (_in_executable && rest.starts_with("*/")) {
      _in_executable = false;
      _at += 2;
      continue;
    }
    if (rest.starts_with("/*+") && _last_kind == TokenKind::word) {
      bool after_keyword = false;
      for (const auto keyword : hint_keywords) after_keyword = after_keyword || same_word(_last_word, keyword);
      if (after_keyword) break;
    }
    if (rest.starts_with("/*!") && !_in_executable) {
      const size_t start = _at;
      size_t at = _at + 3;
      size_t digits = 0;
      while (at + digits < _text.size() && digits < 5 && ascii_digit(_text[at + digits])) ++digits;
      uint32_t version = 0;
      if (digits == 5) {
        for (size_t i = 0; i < 5; ++i) version = version * 10 + static_cast<uint32_t>(_text[at + i] - '0');
        at += 5;
      }
      if (version <= _options.server_version) {
        _in_executable = true;
        _executable_start = start;
        _at = at;
        continue;
      }
      // A version not met: a comment like any other.
    }
    if (rest.starts_with("/*")) {
      const size_t start = _at;
      const auto close = _text.find("*/", _at + 2);
      if (close == std::string_view::npos) fail(start);
      for (size_t i = _at; i < close; ++i)
        if (_text[i] == '\n') ++_line;
      _at = close + 2;
      continue;
    }
    break;
  }
  if (_at >= _text.size() && _in_executable) fail(_executable_start);
}

Token Lexer::next() {
  skip_space_and_comments();
  const size_t start = _at;
  const uint32_t line = _line;
  Token token;
  if (_at >= _text.size()) {
    token.offset = _text.size();
    token.line = _line;
    return token;
  }
  const char c = _text[_at];
  const char after = _at + 1 < _text.size() ? _text[_at + 1] : '\0';
  const bool name_follows_dot = _dot_after_name && start == _last_end;

  if (_text.substr(_at).starts_with("/*+")) {
    const auto close = _text.find("*/", _at + 3);
    if (close == std::string_view::npos) fail(start);
    token = make(TokenKind::hint, std::string(_text.substr(_at + 3, close - _at - 3)), start);
    for (size_t i = _at; i < close; ++i)
      if (_text[i] == '\n') ++_line;
    _at = close + 2;
  } else if (c == '\'') {
    token = quoted('\'', TokenKind::string, start, start + 1);
  } else if (c == '"') {
    token = _options.ansi_quotes ? quoted('"', TokenKind::quoted_identifier, start, start + 1) : quoted('"', TokenKind::string, start, start + 1);
  } else if (c == '`') {
    token = quoted('`', TokenKind::quoted_identifier, start, start + 1);
  } else if ((c == 'n' || c == 'N') && after == '\'' && !name_follows_dot) {
    token = quoted('\'', TokenKind::national_string, start, start + 2);
  } else if ((c == 'x' || c == 'X') && after == '\'' && !name_follows_dot) {
    token = hex_or_bits(TokenKind::hex, start, start + 2);
  } else if ((c == 'b' || c == 'B') && after == '\'' && !name_follows_dot) {
    token = hex_or_bits(TokenKind::bits, start, start + 2);
  } else if (c == '@') {
    token = variable(start);
  } else if (c == '?') {
    ++_at;
    token = make(TokenKind::placeholder, "?", start);
  } else if (word_byte(_at) ||
             (c == '.' && ascii_digit(after) && !(_last_end == start && (_last_kind == TokenKind::word || _last_kind == TokenKind::quoted_identifier)))) {
    token = word_or_number();
  } else {
    token = symbol(start);
  }
  token.offset = start;
  token.length = _at - start;
  token.line = line;

  const bool after_name = _last_end == start && (_last_kind == TokenKind::word || _last_kind == TokenKind::quoted_identifier);
  _dot_after_name = token.kind == TokenKind::symbol && token.value == "." && after_name;
  _last_kind = token.kind;
  _last_word = token.kind == TokenKind::word ? token.value : std::string();
  _last_end = _at;
  return token;
}

Token Lexer::make(TokenKind kind, std::string value, size_t start) const {
  Token token;
  token.kind = kind;
  token.value = std::move(value);
  token.offset = start;
  return token;
}

Token Lexer::word_or_number() {
  const size_t start = _at;
  const auto scan_word = [&] {
    while (word_byte(_at)) _at += utf8_length(_at);
    return make(TokenKind::word, std::string(_text.substr(start, _at - start)), start);
  };
  if (_dot_after_name && start == _last_end) return scan_word();
  const char c = _text[_at];
  if (c == '.') return number(start);
  if (!ascii_digit(c)) return scan_word();

  // 0x and 0b, written in lower case, then digits, then nothing a name holds.
  if (c == '0' && start + 1 < _text.size() && (_text[start + 1] == 'x' || _text[start + 1] == 'b')) {
    const bool hex = _text[start + 1] == 'x';
    size_t end = start + 2;
    while (end < _text.size() && (hex ? hex_digit(_text[end]) : (_text[end] == '0' || _text[end] == '1'))) ++end;
    if (end > start + 2 && !word_byte(end)) {
      _at = end;
      const auto digits = _text.substr(start + 2, end - start - 2);
      return make(hex ? TokenKind::hex : TokenKind::bits, hex ? hex_bytes(digits) : bit_bytes(digits), start);
    }
    return scan_word();
  }

  size_t end = start;
  while (end < _text.size() && ascii_digit(_text[end])) ++end;
  if (end < _text.size() && _text[end] == '.') return number(start);
  if (end < _text.size() && (_text[end] == 'e' || _text[end] == 'E')) {
    size_t at = end + 1;
    if (at < _text.size() && (_text[at] == '+' || _text[at] == '-')) ++at;
    if (at < _text.size() && ascii_digit(_text[at])) {
      while (at < _text.size() && ascii_digit(_text[at])) ++at;
      if (!word_byte(at)) {
        _at = at;
        return make(TokenKind::floating, std::string(_text.substr(start, at - start)), start);
      }
    }
    return scan_word();
  }
  if (word_byte(end)) return scan_word();
  _at = end;
  return make(TokenKind::integer, std::string(_text.substr(start, end - start)), start);
}

// Digits, a point, digits, and an exponent if one follows whole.
Token Lexer::number(size_t start) {
  size_t at = start;
  while (at < _text.size() && ascii_digit(_text[at])) ++at;
  ++at;  // the point
  while (at < _text.size() && ascii_digit(_text[at])) ++at;
  auto kind = TokenKind::decimal;
  if (at < _text.size() && (_text[at] == 'e' || _text[at] == 'E')) {
    size_t exponent = at + 1;
    if (exponent < _text.size() && (_text[exponent] == '+' || _text[exponent] == '-')) ++exponent;
    if (exponent < _text.size() && ascii_digit(_text[exponent])) {
      while (exponent < _text.size() && ascii_digit(_text[exponent])) ++exponent;
      at = exponent;
      kind = TokenKind::floating;
    }
  }
  _at = at;
  return make(kind, std::string(_text.substr(start, at - start)), start);
}

Token Lexer::quoted(char quote, TokenKind kind, size_t start, size_t content) {
  const bool escapes = quote != '`' && kind != TokenKind::quoted_identifier && !_options.no_backslash_escapes;
  const bool name = quote == '`' || kind == TokenKind::quoted_identifier;
  std::string value;
  size_t at = content;
  uint32_t lines = 0;
  for (;;) {
    if (at >= _text.size()) fail(start);
    const char c = _text[at];
    if (escapes && c == '\\') {
      if (at + 1 >= _text.size()) fail(start);
      const char e = _text[at + 1];
      switch (e) {
        case '0':
          value.push_back('\0');
          break;
        case 'b':
          value.push_back('\b');
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 'r':
          value.push_back('\r');
          break;
        case 't':
          value.push_back('\t');
          break;
        case 'Z':
          value.push_back('\x1a');
          break;
        case '%':
        case '_':
          value.push_back('\\');
          value.push_back(e);
          break;
        default:
          value.push_back(e);
          if (e == '\n') ++lines;
          break;
      }
      at += 2;
      continue;
    }
    if (c == quote) {
      if (at + 1 < _text.size() && _text[at + 1] == quote) {
        value.push_back(quote);
        at += 2;
        continue;
      }
      ++at;
      break;
    }
    if (name) {
      // A name is UTF-8 with no NUL.
      const auto length = c == '\0' ? 0 : utf8_length(at);
      if (length == 0) fail(at);
      if (c == '\n') ++lines;
      value.append(_text.substr(at, length));
      at += length;
      continue;
    }
    if (c == '\n') ++lines;
    value.push_back(c);
    ++at;
  }
  _at = at;
  _line += lines;
  return make(kind, std::move(value), start);
}

Token Lexer::hex_or_bits(TokenKind kind, size_t start, size_t content) {
  const auto close = _text.find('\'', content);
  if (close == std::string_view::npos) fail(start);
  const auto digits = _text.substr(content, close - content);
  for (const char d : digits)
    if (kind == TokenKind::hex ? !hex_digit(d) : (d != '0' && d != '1')) fail(start);
  if (kind == TokenKind::hex && digits.size() % 2) fail(start);
  _at = close + 1;
  return make(kind, kind == TokenKind::hex ? hex_bytes(digits) : bit_bytes(digits), start);
}

Token Lexer::variable(size_t start) {
  const size_t at = start + 1;
  if (at < _text.size() && _text[at] == '@') {
    _at = at + 1;
    return make(TokenKind::symbol, "@@", start);
  }
  if (at < _text.size() && (_text[at] == '\'' || _text[at] == '"' || _text[at] == '`')) {
    const char quote = _text[at];
    auto token = quoted(quote, quote == '`' ? TokenKind::quoted_identifier : TokenKind::string, start, at + 1);
    token.kind = TokenKind::user_variable;
    return token;
  }
  size_t end = at;
  while (end < _text.size() && (word_byte(end) || _text[end] == '.')) end += _text[end] == '.' ? 1 : utf8_length(end);
  if (end == at) fail(start);
  _at = end;
  return make(TokenKind::user_variable, std::string(_text.substr(at, end - at)), start);
}

Token Lexer::symbol(size_t start) {
  const auto rest = _text.substr(start);
  for (const auto s : symbols) {
    if (rest.starts_with(s)) {
      _at = start + s.size();
      return make(TokenKind::symbol, std::string(s), start);
    }
  }
  fail(start);
}

}  // namespace kvasir::sql
