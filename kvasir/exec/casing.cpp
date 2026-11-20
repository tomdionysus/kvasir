//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/casing.h"

#include <algorithm>
#include <span>

#include "kvasir/exec/casing_table.h"
#include "kvasir/exec/variables.h"

namespace kvasir::exec {

namespace {

std::span<const casing::CasePair> table(Casing c) {
  if (c == Casing::unicode9) return {casing::unicode9, casing::unicode9_size};
  if (c == Casing::unicode3) return {casing::unicode3, casing::unicode3_size};
  return {};
}

// One well-formed UTF-8 character at `at`: its code point and length, or a
// length of 0 for bytes that are not one.
std::pair<char32_t, size_t> decode(std::string_view s, size_t at) {
  const auto byte = [&](size_t i) { return static_cast<uint8_t>(s[at + i]); };
  const auto follows = [&](size_t i) { return at + i < s.size() && (byte(i) & 0xc0) == 0x80; };
  const uint8_t lead = byte(0);
  if (lead < 0x80) return {lead, 1};
  if (lead >= 0xc2 && lead < 0xe0 && follows(1)) return {static_cast<char32_t>(((lead & 0x1f) << 6) | (byte(1) & 0x3f)), 2};
  if (lead >= 0xe0 && lead < 0xf0 && follows(1) && follows(2)) {
    const char32_t c = ((lead & 0x0f) << 12) | ((byte(1) & 0x3f) << 6) | (byte(2) & 0x3f);
    if (c >= 0x800 && (c < 0xd800 || c > 0xdfff)) return {c, 3};
  }
  if (lead >= 0xf0 && lead < 0xf5 && follows(1) && follows(2) && follows(3)) {
    const char32_t c = ((lead & 0x07) << 18) | ((byte(1) & 0x3f) << 12) | ((byte(2) & 0x3f) << 6) | (byte(3) & 0x3f);
    if (c >= 0x10000 && c <= 0x10ffff) return {c, 4};
  }
  return {0, 0};
}

void encode(char32_t c, std::string& out) {
  if (c < 0x80) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800) {
    out.push_back(static_cast<char>(0xc0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
  } else if (c < 0x10000) {
    out.push_back(static_cast<char>(0xe0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
  }
}

// As MySQL maps (observed): in place, in a buffer the argument's length.
// Each character is read from the buffer as it now stands, so one written
// longer over the next character's bytes leaves bytes that are not a
// character, and the result ends there; so does one that no longer fits.
template <typename Pick>
std::string mapped(std::string_view s, Casing casing, Pick pick) {
  const auto pairs = table(casing);
  if (pairs.empty()) return std::string(s);
  std::string buffer(s);
  size_t from = 0, to = 0;
  std::string written;
  while (from < buffer.size()) {
    const auto [c, length] = decode(buffer, from);
    if (length == 0) break;
    const auto it = std::lower_bound(pairs.begin(), pairs.end(), c, [](const casing::CasePair& p, char32_t code) { return p.code < code; });
    written.clear();
    encode(it != pairs.end() && it->code == c ? pick(*it) : c, written);
    if (to + written.size() > buffer.size()) break;
    buffer.replace(to, written.size(), written);
    from += length;
    to += written.size();
  }
  buffer.resize(to);
  return buffer;
}

}  // namespace

Casing casing_of(uint16_t collation) {
  const auto* info = find_collation(collation);
  if (!info || info->charset == "binary") return Casing::none;
  // utf8mb4's 0900 collations use Unicode 9.0.0's; every other, 3.0.0's.
  if (info->name.find("_0900_") != std::string::npos) return Casing::unicode9;
  return Casing::unicode3;
}

std::string upper(std::string_view utf8, Casing casing) {
  return mapped(utf8, casing, [](const casing::CasePair& p) { return p.upper; });
}

std::string lower(std::string_view utf8, Casing casing) {
  return mapped(utf8, casing, [](const casing::CasePair& p) { return p.lower; });
}

}  // namespace kvasir::exec
