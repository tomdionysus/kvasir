//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstdint>
#include <string>
#include <vector>

#include "kvasir/exec/collation.h"

namespace kvasir::exec {

namespace {

// The length of the UTF-8 character at `i`; a byte that begins none is a
// character of its own.
size_t utf8_length(std::string_view s, size_t i) {
  const auto lead = static_cast<uint8_t>(s[i]);
  const size_t n = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
  if (i + n > s.size()) return 1;
  for (size_t k = 1; k < n; ++k)
    if ((static_cast<uint8_t>(s[i + k]) & 0xc0) != 0x80) return 1;
  return n;
}

std::vector<std::string_view> characters(std::string_view s, bool bytes) {
  std::vector<std::string_view> out;
  for (size_t i = 0; i < s.size();) {
    const size_t n = bytes ? 1 : utf8_length(s, i);
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}

struct Token {
  enum class Kind { literal, one, any } kind;
  std::string key;  // a literal's sort key
};

}  // namespace

bool Collation::like(std::string_view subject, std::string_view pattern, std::string_view escape) const {
  const bool bytes = characters_are_bytes();
  // Under a collation two characters are equal when their sort keys are.
  const auto key = [&](std::string_view c) { return bytes ? std::string(c) : sort_key(c); };

  std::vector<Token> tokens;
  const auto parts = characters(pattern, bytes);
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!escape.empty() && parts[i] == escape && i + 1 < parts.size()) {
      tokens.push_back({Token::Kind::literal, key(parts[++i])});
    } else if (parts[i] == "%") {
      if (tokens.empty() || tokens.back().kind != Token::Kind::any) tokens.push_back({Token::Kind::any, {}});
    } else if (parts[i] == "_") {
      tokens.push_back({Token::Kind::one, {}});
    } else {
      tokens.push_back({Token::Kind::literal, key(parts[i])});
    }
  }
  std::vector<std::string> keys;
  for (const auto c : characters(subject, bytes)) keys.push_back(key(c));

  // Greedy, going back to the last '%' on a mismatch: O(subject x pattern).
  size_t s = 0, p = 0, star = tokens.size(), mark = 0;
  while (s < keys.size()) {
    if (p < tokens.size() && (tokens[p].kind == Token::Kind::one || (tokens[p].kind == Token::Kind::literal && tokens[p].key == keys[s]))) {
      ++s;
      ++p;
    } else if (p < tokens.size() && tokens[p].kind == Token::Kind::any) {
      star = p++;
      mark = s;
    } else if (star < tokens.size()) {
      p = star + 1;
      s = ++mark;
    } else {
      return false;
    }
  }
  while (p < tokens.size() && tokens[p].kind == Token::Kind::any) ++p;
  return p == tokens.size();
}

}  // namespace kvasir::exec
