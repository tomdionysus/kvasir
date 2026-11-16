//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Sort keys and comparisons as MySQL 8.4.13 gives them (WEIGHT_STRING() and
// =, observed on the reference server, 2026-11-16), under UTS #10 and
// Unicode's table for UCA 9.0.0 (ruling 32). Inputs are given as UTF-8 bytes.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kvasir/exec/collation.h"

namespace kvasir::exec {
namespace {

std::string bytes(std::string_view hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  return out;
}

std::string hex(const std::string& s) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : s) {
    out.push_back(digits[static_cast<uint8_t>(c) >> 4]);
    out.push_back(digits[static_cast<uint8_t>(c) & 0xf]);
  }
  return out;
}

constexpr uint16_t ai_ci = 255, as_cs = 278, bin = 46, bin_0900 = 309;

struct Key {
  const char* utf8;
  const char* ai_ci;
  const char* as_cs;
};

TEST(ExecCollation, SortKeysAreMySqls) {
  const std::vector<Key> keys = {
      {"61", "1C47", "1C470000002000000002"},                                      // a
      {"41", "1C47", "1C470000002000000008"},                                      // A
      {"C3A1", "1C47", "1C47000000200024000000020002"},                            // á
      {"61CC81", "1C47", "1C47000000200024000000020002"},                          // a + combining acute
      {"C39F", "1E711E71", "1E711E7100000020011000200000000400040004"},            // ß
      {"C386", "1C471CAA", "1C471CAA00000020011000200000000A0004000A"},            // Æ
      {"C786", "1C8F1F21", "1C8F1F2100000020002000280000000400040004"},            // ǆ
      {"ED959C", "3C073C733CD4", "3C073C733CD400000020002000200000000200020002"},  // 한, a Hangul syllable
      {"E4B8AD", "FB40CE2D", nullptr},                                             // 中, core Han
      {"E39080", "FB80B400", "FB80B4000000002000000002"},                          // 㐀, Han extension A
      {"F0908080", "45E7", "45E70000002000000002"},                                // a Linear B syllable
      {"F1908080", "FBCA8000", "FBCA80000000002000000002"},                        // U+50000, unassigned
      {"F0979C80", "FB008700", "FB0087000000002000000002"},                        // U+17700, Tangut
      {"4CC2B7", "1D77028B", "1D77028B000000200020000000080002"},                  // L and a middle dot
      {"2D", "020D", "020D0000002000000002"},                                      // -
      {"20", "0209", "02090000002000000002"},                                      // space
      {"00", "", nullptr},                                                         // NUL, ignorable
      {"0D0A", "02050202", "02050202000000200020000000020002"},                    // CR LF
      {"E28093", "0215", "02150000002000000002"},                                  // en dash
      {"CEA9", "1FE1", "1FE10000002000000008"},                                    // Ω
      {"D0AF", "21AF", "21AF0000002000000008"},                                    // Я
      {"D985", "23A3", "23A30000002000000002"},                                    // م
      {"E0A495", "2676", "26760000002000000002"},                                  // क
      {"416220", "1C471C600209", nullptr},                                         // "Ab "
      // The table's contractions are not applied: each code point weighs alone, in order.
      {"D098CC86", "2080", nullptr},          // И and a combining breve
      {"D099", "208D", nullptr},              // Й
      {"E0B980E0B881", "2DAD2D73", nullptr},  // Thai sara e, ko
      {"E0BB80E0BA81", "2DDE2DB4", nullptr},  // Lao vowel e, ko
      {"EAAAB5EAAA80", "2E182DE3", nullptr},  // Tai Viet e, low ko
      {"E0BEB2E0BE80", "2E602E79", nullptr},  // Tibetan
      {"E0AE92E0AF97", "27CB27F2", nullptr},  // Tamil o, au length mark
  };
  const auto* insensitive = collation_by_id(ai_ci);
  const auto* sensitive = collation_by_id(as_cs);
  ASSERT_TRUE(insensitive);
  ASSERT_TRUE(sensitive);
  for (const auto& k : keys) {
    SCOPED_TRACE(k.utf8);
    EXPECT_EQ(hex(insensitive->sort_key(bytes(k.utf8))), k.ai_ci);
    if (k.as_cs) {
      EXPECT_EQ(hex(sensitive->sort_key(bytes(k.utf8))), k.as_cs);
    }
  }
  EXPECT_EQ(hex(sensitive->sort_key("Ab")), "1C471C60000000200020000000080002");
}

TEST(ExecCollation, AccentAndCaseInsensitiveNoPad) {
  const auto* c = collation_by_id(ai_ci);
  EXPECT_EQ(c->compare("a", "A"), 0);
  EXPECT_EQ(c->compare("a", bytes("C3A1")), 0);
  EXPECT_EQ(c->compare(bytes("C39F"), "ss"), 0);
  EXPECT_EQ(c->compare(bytes("C386"), "AE"), 0);
  EXPECT_EQ(c->compare(bytes("C785"), bytes("44C5BD")), 0);  // ǅ and D followed by Ž
  EXPECT_NE(c->compare("a", "a "), 0);
  EXPECT_NE(c->compare("", " "), 0);
  EXPECT_NE(c->compare("a-b", "ab"), 0);
  EXPECT_LT(c->compare("a", "B"), 0);
  EXPECT_GT(c->compare("b", "A"), 0);
  EXPECT_LT(c->compare("", "a"), 0);
  EXPECT_EQ(c->compare(std::string("a\0", 2), "a"), 0);
}

TEST(ExecCollation, AccentAndCaseSensitive) {
  const auto* c = collation_by_id(as_cs);
  EXPECT_NE(c->compare("a", "A"), 0);
  EXPECT_NE(c->compare("a", bytes("C3A1")), 0);
  EXPECT_EQ(c->compare("a", "a"), 0);
  EXPECT_LT(c->compare("a", "A"), 0);
  // Primaries first: a < B whatever the case.
  EXPECT_LT(c->compare("A", "b"), 0);
}

TEST(ExecCollation, BinaryCollations) {
  const auto* padded = collation_by_id(bin);
  ASSERT_TRUE(padded);
  EXPECT_EQ(padded->compare("a", "a "), 0);
  EXPECT_NE(padded->compare("a", "A"), 0);
  EXPECT_GT(padded->compare("a", "A"), 0);
  const auto* no_pad = collation_by_id(bin_0900);
  ASSERT_TRUE(no_pad);
  EXPECT_NE(no_pad->compare("a", "a "), 0);
  EXPECT_LT(no_pad->compare("a", "b"), 0);
}

TEST(ExecCollation, CollationsNotYetBuiltAreNone) {
  EXPECT_FALSE(collation_by_id(45));   // utf8mb4_general_ci
  EXPECT_FALSE(collation_by_id(224));  // utf8mb4_unicode_ci
  EXPECT_FALSE(collation_by_id(9999));
}

TEST(ExecCollation, BytesThatAreNotUtf8CountAsReplacementCharacters) {
  const auto* c = collation_by_id(ai_ci);
  EXPECT_EQ(c->compare("\xff", bytes("EFBFBD")), 0);
  EXPECT_EQ(c->sort_key("\xc3"), c->sort_key(bytes("EFBFBD")));
}

}  // namespace
}  // namespace kvasir::exec
