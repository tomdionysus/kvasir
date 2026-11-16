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
#include <utility>
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
  // The binary character set's collation: bytes, NO PAD.
  const auto* bytes_only = collation_by_id(63);
  ASSERT_TRUE(bytes_only);
  EXPECT_NE(bytes_only->compare("a", "a "), 0);
  EXPECT_LT(bytes_only->compare("A", "a"), 0);
}

// PAD SPACE pads the shorter string with spaces rather than trimming, so a
// character below a space sorts before the end of a string (observed).
TEST(ExecCollation, PadSpacePadsRatherThanTrims) {
  for (const uint16_t id : {uint16_t{46}, uint16_t{45}, uint16_t{33}}) {
    SCOPED_TRACE(id);
    const auto* c = collation_by_id(id);
    ASSERT_TRUE(c);
    EXPECT_LT(c->compare("a\t", "a"), 0);
    EXPECT_GT(c->compare("a", "a\t"), 0);
    EXPECT_EQ(c->compare("a", "a  "), 0);
  }
}

TEST(ExecCollation, BinarySortKeys) {
  EXPECT_EQ(hex(collation_by_id(bin)->sort_key("a ")), "000061000020");
  EXPECT_EQ(hex(collation_by_id(bin)->sort_key(bytes("C3A9"))), "0000E9");
  EXPECT_EQ(hex(collation_by_id(bin_0900)->sort_key("a ")), "6120");
  EXPECT_EQ(hex(collation_by_id(bin_0900)->sort_key(bytes("C3A9"))), "C3A9");
  EXPECT_EQ(hex(collation_by_id(63)->sort_key("a ")), "6120");
}

// Observed WEIGHT_STRING() under utf8mb4_general_ci and utf8mb3_general_ci,
// which agree on every BMP code point (ruling 33).
TEST(ExecCollation, GeneralCiSortKeys) {
  const std::vector<std::pair<const char*, const char*>> keys = {
      {"61", "0041"},              // a
      {"41", "0041"},              // A
      {"C3A0", "0041"},            // à
      {"C3A9", "0045"},            // é
      {"C39F", "0053"},            // ß, one S
      {"D0B9", "0419"},            // й keeps its breve
      {"D191", "0415"},            // ё
      {"C680", "0180"},            // ƀ, no case pair in Unicode 3.0
      {"D8A3", "0623"},            // Arabic alef with hamza, not stripped
      {"E3818C", "304C"},          // hiragana ga, not stripped
      {"E289A0", "2260"},          // not equal to, not stripped
      {"F09F9880", "FFFD"},        // past the BMP
      {"616220", "004100420020"},  // "ab "
      {"00", "0000"},
  };
  for (const uint16_t id : {uint16_t{45}, uint16_t{33}}) {
    const auto* c = collation_by_id(id);
    ASSERT_TRUE(c);
    for (const auto& [utf8, key] : keys) {
      SCOPED_TRACE(utf8);
      EXPECT_EQ(hex(c->sort_key(bytes(utf8))), key);
    }
  }
  const auto* c = collation_by_id(45);
  EXPECT_EQ(c->compare("a", "A"), 0);
  EXPECT_EQ(c->compare(bytes("C39F"), "s"), 0);
  EXPECT_NE(c->compare(bytes("C39F"), "ss"), 0);
  EXPECT_NE(c->compare(bytes("D0B9"), bytes("D0B8")), 0);  // й and и
  EXPECT_LT(c->compare("a", "B"), 0);
}

// LIKE, observed on MySQL 8.4.13, 2026-11-16: one character at a time, each
// pair compared under the collation, never padded; under the binary
// character set a character is a byte.
TEST(ExecCollation, Like) {
  struct Case {
    uint16_t collation;
    std::string subject, pattern, escape;
    bool matches;
  };
  const std::string sharp_s = bytes("C39F"), a_acute = bytes("C3A1"), ae = bytes("C386"), dz = bytes("C786"), dz_title = bytes("C785");
  const std::string e_acute = bytes("C3A9"), emoji = bytes("F09F9880");
  const std::vector<Case> cases = {
      {ai_ci, "a", "A", "\\", true},
      {ai_ci, a_acute, "a", "\\", true},
      {ai_ci, sharp_s, "ss", "\\", false},
      {ai_ci, "ss", sharp_s, "\\", false},
      {ai_ci, sharp_s, "_", "\\", true},
      {ai_ci, "ss", "_", "\\", false},
      {ai_ci, "a ", "a", "\\", false},
      {ai_ci, "a", "a ", "\\", false},
      {ai_ci, "abc", "a%", "\\", true},
      {ai_ci, "abc", "%c", "\\", true},
      {ai_ci, "abc", "a_c", "\\", true},
      {ai_ci, "abc", "a__c", "\\", false},
      {ai_ci, "", "%", "\\", true},
      {ai_ci, "", "_", "\\", false},
      {ai_ci, "a%c", "a\\%c", "\\", true},
      {ai_ci, "abc", "a\\%c", "\\", false},
      {ai_ci, "a_c", "a|_c", "|", true},
      {ai_ci, ae, "AE", "\\", false},
      {ai_ci, ae, ae, "\\", true},
      {ai_ci, "x", "%%%x", "\\", true},
      {ai_ci, "a\\b", "a\\\\b", "\\", true},
      {ai_ci, "ab", "a\\", "\\", false},
      {ai_ci, dz, "d" + bytes("C5BE"), "\\", false},
      {ai_ci, dz, "_", "\\", true},
      {ai_ci, dz, "__", "\\", false},
      {ai_ci, dz_title, dz, "\\", true},
      {ai_ci, emoji, "_", "\\", true},
      {ai_ci, "a", "a%%", "\\", true},
      {ai_ci, "abc", "A%C", "\\", true},
      {ai_ci, e_acute, "e_", "\\", false},
      {ai_ci, e_acute, "e", "\\", true},
      // No escape: a backslash is itself.
      {ai_ci, "a\\b", "a\\_", "", true},
      {ai_ci, "a%c", "a\\%c", "", false},
      {bin, "a", "A", "\\", false},
      {bin, "a ", "a", "\\", false},
      {bin, "a", "a ", "\\", false},
      {bin, e_acute, "_", "\\", true},
      {45, "a", "A", "\\", true},
      {45, sharp_s, "s", "\\", true},
      {45, "a ", "a", "\\", false},
      {45, e_acute, "E", "\\", true},
      {63, "a", "A", "\\", false},
      {63, e_acute, "_", "\\", false},
      {63, e_acute, "__", "\\", true},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(std::to_string(c.collation) + " " + hex(c.subject) + " LIKE " + hex(c.pattern));
    EXPECT_EQ(collation_by_id(c.collation)->like(c.subject, c.pattern, c.escape), c.matches);
  }
}

TEST(ExecCollation, CollationsNotYetBuiltAreNone) {
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
