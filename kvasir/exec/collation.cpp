//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/collation.h"

#include <algorithm>
#include <array>
#include <tuple>
#include <vector>

#include "kvasir/exec/ducet.h"

namespace kvasir::exec {

namespace {

constexpr char32_t replacement = 0xFFFD;

// The code point at `i`, moving `i` past it; a byte that begins no UTF-8
// character is U+FFFD and one byte long.
char32_t decode(std::string_view s, size_t& i) {
  const auto byte = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
  const uint8_t lead = byte(i);
  size_t length = 0;
  char32_t cp = 0;
  if (lead < 0x80) {
    ++i;
    return lead;
  }
  if ((lead & 0xe0) == 0xc0) {
    length = 2;
    cp = lead & 0x1f;
  } else if ((lead & 0xf0) == 0xe0) {
    length = 3;
    cp = lead & 0x0f;
  } else if ((lead & 0xf8) == 0xf0) {
    length = 4;
    cp = lead & 0x07;
  } else {
    ++i;
    return replacement;
  }
  if (i + length > s.size()) {
    ++i;
    return replacement;
  }
  for (size_t k = 1; k < length; ++k) {
    if ((byte(i + k) & 0xc0) != 0x80) {
      ++i;
      return replacement;
    }
    cp = (cp << 6) | (byte(i + k) & 0x3f);
  }
  if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
    ++i;
    return replacement;
  }
  i += length;
  return cp;
}

// The string's code points, Hangul syllables decomposed to their jamo.
std::vector<char32_t> code_points(std::string_view s) {
  std::vector<char32_t> out;
  for (size_t i = 0; i < s.size();) {
    const char32_t cp = decode(s, i);
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
      const char32_t index = cp - 0xAC00;
      out.push_back(0x1100 + index / 588);
      out.push_back(0x1161 + (index % 588) / 28);
      if (index % 28) out.push_back(0x11A7 + index % 28);
    } else {
      out.push_back(cp);
    }
  }
  return out;
}

const ducet::Entry* find(char32_t a, char32_t b, char32_t c) {
  const auto* begin = ducet::entries;
  const auto* end = ducet::entries + ducet::entry_count;
  const auto less = [](const ducet::Entry& e, const std::array<uint32_t, 3>& key) {
    return std::tie(e.first, e.second, e.third) < std::tie(key[0], key[1], key[2]);
  };
  const std::array<uint32_t, 3> key{a, b, c};
  const auto* it = std::lower_bound(begin, end, key, less);
  if (it != end && it->first == a && it->second == b && it->third == c) return it;
  return nullptr;
}

bool core_han(char32_t cp) {
  if (cp >= 0x4E00 && cp <= 0x9FD5) return true;
  switch (cp) {
    case 0xFA0E:
    case 0xFA0F:
    case 0xFA11:
    case 0xFA13:
    case 0xFA14:
    case 0xFA1F:
    case 0xFA21:
    case 0xFA23:
    case 0xFA24:
    case 0xFA27:
    case 0xFA28:
    case 0xFA29:
      return true;
    default:
      return false;
  }
}

bool other_han(char32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DB5) || (cp >= 0x20000 && cp <= 0x2A6D6) || (cp >= 0x2A700 && cp <= 0x2B734) || (cp >= 0x2B740 && cp <= 0x2B81D) ||
         (cp >= 0x2B820 && cp <= 0x2CEA1);
}

bool tangut(char32_t cp) { return (cp >= 0x17000 && cp <= 0x187EC) || (cp >= 0x18800 && cp <= 0x18AF2); }

// UTS #10, "Implicit Weights", for a code point the table does not list.
void implicit(char32_t cp, std::vector<ducet::Element>& out) {
  uint32_t first = 0;
  uint32_t second = 0;
  if (tangut(cp)) {
    first = 0xFB00;
    second = ((cp - 0x17000) & 0x7FFF) | 0x8000;
  } else {
    const uint32_t base = core_han(cp) ? 0xFB40 : other_han(cp) ? 0xFB80 : 0xFBC0;
    first = base + (cp >> 15);
    second = (cp & 0x7FFF) | 0x8000;
  }
  out.push_back({static_cast<uint16_t>(first), 0x0020, 0x0002});
  out.push_back({static_cast<uint16_t>(second), 0, 0});
}

void weigh(char32_t cp, std::vector<ducet::Element>& out) {
  if (const auto* entry = find(cp, 0, 0)) {
    out.insert(out.end(), ducet::elements + entry->offset, ducet::elements + entry->offset + entry->count);
  } else {
    implicit(cp, out);
  }
}

// The collation elements, each code point weighed alone in the order
// written, as MySQL's 0900 collations weigh them (observed: none of the
// table's contractions applies, L·, И with a breve, Thai, Lao and Tai Viet
// prevowels, Tibetan and Tamil sequences included).
std::vector<ducet::Element> elements(std::string_view s) {
  std::vector<ducet::Element> out;
  for (const char32_t cp : code_points(s)) weigh(cp, out);
  return out;
}

void put(std::string& key, uint16_t weight) {
  key.push_back(static_cast<char>(weight >> 8));
  key.push_back(static_cast<char>(weight & 0xff));
}

int sign(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }

int compare_keys(const std::string& a, const std::string& b) { return sign(a.compare(b)); }

class Uca : public Collation {
 public:
  explicit Uca(int levels) : _levels(levels) {}
  int compare(std::string_view a, std::string_view b) const override { return compare_keys(sort_key(a), sort_key(b)); }
  std::string sort_key(std::string_view s) const override {
    const auto all = elements(s);
    std::string key;
    for (int level = 0; level < _levels; ++level) {
      if (level > 0) put(key, 0);
      for (const auto& e : all) {
        const uint16_t w = level == 0 ? e.primary : level == 1 ? e.secondary : e.tertiary;
        if (w) put(key, w);
      }
    }
    return key;
  }

 private:
  int _levels;
};

class Binary : public Collation {
 public:
  explicit Binary(bool pad) : _pad(pad) {}
  int compare(std::string_view a, std::string_view b) const override { return compare_keys(sort_key(a), sort_key(b)); }
  // UTF-8's byte order is its code points' order.
  std::string sort_key(std::string_view s) const override {
    if (_pad)
      while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return std::string(s);
  }

 private:
  bool _pad;
};

}  // namespace

const Collation* collation_by_id(uint16_t id) {
  static const Uca accent_and_case_insensitive(1);
  static const Uca accent_and_case_sensitive(3);
  static const Binary padded(true);
  static const Binary unpadded(false);
  switch (id) {
    case 255:  // utf8mb4_0900_ai_ci
      return &accent_and_case_insensitive;
    case 278:  // utf8mb4_0900_as_cs
      return &accent_and_case_sensitive;
    case 46:  // utf8mb4_bin
      return &padded;
    case 309:  // utf8mb4_0900_bin
    case 63:   // binary
      return &unpadded;
    default:
      return nullptr;
  }
}

}  // namespace kvasir::exec
