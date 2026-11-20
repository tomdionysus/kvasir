//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// UPPER() and LOWER()'s case mapping, held to what MySQL 8.4.13 answers for
// every code point (differential/casing.tsv, recorded on the reference
// server): Unicode 9.0.0's simple mappings under utf8mb4's 0900 collations
// (probed for _as_cs and _bin too: U+AB70 and U+0180 mapped); Unicode
// 3.0.0's, in the BMP only, under every other collation (utf8mb4_bin and
// utf8mb4_general_ci agree on all 2,621 code points; utf8mb3_bin and
// utf8mb4_unicode_ci probed: U+01F9 mapped, U+AB70, U+0180, U+2C30, U+1E9E
// not); none for binary. MySQL maps in place, in a buffer the length of its
// argument: a character read from it after a longer one was written over
// it, or one that no longer fits, ends the result (probed).

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "kvasir/exec/casing.h"

namespace kvasir::exec {
namespace {

std::string utf8(char32_t c) {
  std::string out;
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
  return out;
}

std::string hex(std::string_view bytes) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  for (const char ch : bytes) {
    out.push_back(digits[static_cast<uint8_t>(ch) >> 4]);
    out.push_back(digits[static_cast<uint8_t>(ch) & 15]);
  }
  return out;
}

TEST(ExecCasing, EachCollationsMapping) {
  EXPECT_EQ(casing_of(255), Casing::unicode9);  // utf8mb4_0900_ai_ci
  EXPECT_EQ(casing_of(278), Casing::unicode9);  // utf8mb4_0900_as_cs
  EXPECT_EQ(casing_of(309), Casing::unicode9);  // utf8mb4_0900_bin
  EXPECT_EQ(casing_of(46), Casing::unicode3);   // utf8mb4_bin
  EXPECT_EQ(casing_of(45), Casing::unicode3);   // utf8mb4_general_ci
  EXPECT_EQ(casing_of(224), Casing::unicode3);  // utf8mb4_unicode_ci
  EXPECT_EQ(casing_of(33), Casing::unicode3);   // utf8mb3_general_ci
  EXPECT_EQ(casing_of(83), Casing::unicode3);   // utf8mb3_bin
  EXPECT_EQ(casing_of(63), Casing::none);       // binary
  EXPECT_EQ(upper("aé\xc3\x9f", Casing::unicode9), "AÉ\xc3\x9f");
  EXPECT_EQ(upper("ab", Casing::none), "ab");
}

// Probed on MySQL 8.4.13 under utf8mb4_0900_ai_ci: U+023A (2 bytes) lowers
// to U+2C65 (3 bytes), U+0250 uppers to U+2C6F.
TEST(ExecCasing, InPlaceAsMySql) {
  const auto low = [](const std::string& hex_in) {
    std::string bytes;
    for (size_t i = 0; i + 1 < hex_in.size(); i += 2) bytes.push_back(static_cast<char>(std::stoi(hex_in.substr(i, 2), nullptr, 16)));
    return hex(lower(bytes, Casing::unicode9));
  };
  EXPECT_EQ(low("C8BA"), "");
  EXPECT_EQ(low("61C8BA62"), "61E2B1A5");
  EXPECT_EQ(low("C8BA62"), "E2B1A5");
  EXPECT_EQ(low("62C8BA"), "62");
  EXPECT_EQ(low("C8BAC8BA"), "E2B1A5");
  EXPECT_EQ(low("C8BAE2B1A5"), "E2B1A5");
  EXPECT_EQ(hex(upper("\xe2\xb1\xa5"
                      "a",
                      Casing::unicode9)),
            "C8BA41");
  EXPECT_EQ(hex(upper("\xc9\x90\xc9\x90\xc9\x90", Casing::unicode9)), "E2B1AF");
}

TEST(ExecCasing, EveryCodePointAsMySql) {
  std::ifstream in(std::string(KVASIR_DIFFERENTIAL_DIR) + "/casing.tsv");
  ASSERT_TRUE(in);
  std::set<char32_t> listed;
  size_t rows = 0, wrong = 0;
  const auto check = [&](const std::string& got, const std::string& want, const char* what, char32_t cp) {
    if (got == want) return;
    if (++wrong <= 20) ADD_FAILURE() << what << " of U+" << std::hex << static_cast<uint32_t>(cp) << ": " << got << ", MySQL " << want;
  };
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    std::istringstream split(line);
    std::string field;
    while (std::getline(split, field, '\t')) f.push_back(field);
    ASSERT_EQ(f.size(), 8u) << line;
    const auto cp = static_cast<char32_t>(std::stoul(f[0]));
    const auto c = utf8(cp);
    ASSERT_EQ(hex(c), f[7]) << line;
    listed.insert(cp);
    ++rows;
    check(hex(upper(c, Casing::unicode9)), f[1], "UPPER under 0900_ai_ci", cp);
    check(hex(lower(c, Casing::unicode9)), f[2], "LOWER under 0900_ai_ci", cp);
    check(hex(upper(c, Casing::unicode3)), f[3], "UPPER under general_ci", cp);
    check(hex(lower(c, Casing::unicode3)), f[4], "LOWER under general_ci", cp);
    check(hex(upper(c, Casing::unicode3)), f[5], "UPPER under utf8mb4_bin", cp);
    check(hex(lower(c, Casing::unicode3)), f[6], "LOWER under utf8mb4_bin", cp);
  }
  EXPECT_EQ(rows, 2621u);
  // Every other code point is unchanged by all three.
  for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
    if ((cp >= 0xD800 && cp <= 0xDFFF) || listed.count(cp)) continue;
    const auto c = utf8(cp);
    check(upper(c, Casing::unicode9), c, "UPPER (unchanged)", cp);
    check(lower(c, Casing::unicode9), c, "LOWER (unchanged)", cp);
    check(upper(c, Casing::unicode3), c, "UPPER under general_ci (unchanged)", cp);
    check(lower(c, Casing::unicode3), c, "LOWER under general_ci (unchanged)", cp);
  }
  EXPECT_EQ(wrong, 0u);
}

}  // namespace
}  // namespace kvasir::exec
