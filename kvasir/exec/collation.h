//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// How strings compare under a collation (ruling 32): the Unicode Collation
// Algorithm over Unicode's table for UCA 9.0.0, compiled at build time from
// kvasir/data/unicode/allkeys-9.0.0.txt, written here by hand. As MySQL 8.4's
// utf8mb4_0900_* collations do (observed on the reference server): every
// character weighs, spaces and punctuation included (no variable weighting);
// code points the table does not list take UCA's implicit weights (core Han,
// other Han, Tangut, unassigned); Hangul syllables are decomposed to their
// jamo; each code point weighs alone, in the order written, for none of the
// table's contractions applies (Thai and Lao prevowels included); the 0900
// collations are NO PAD, so trailing spaces count.
//
// utf8mb4_0900_ai_ci compares primary weights only; utf8mb4_0900_as_cs
// primaries, then secondaries, then tertiaries. utf8mb4_bin and
// utf8mb4_0900_bin compare code points, the first ignoring trailing spaces
// (PAD SPACE); binary, the binary character set's, compares bytes, NO PAD.
// Bytes that are not UTF-8 count as U+FFFD under the UCA collations.
namespace kvasir::exec {

class Collation {
 public:
  virtual ~Collation() = default;
  // -1, 0 or 1.
  virtual int compare(std::string_view a, std::string_view b) const = 0;
  // What MySQL's WEIGHT_STRING() gives: the weights, two bytes each, high
  // byte first, levels after the first each preceded by 0000.
  virtual std::string sort_key(std::string_view s) const = 0;
};

// The collation with this id, or nothing for one Kvasir cannot yet compare
// under (utf8mb4_general_ci and utf8mb4_unicode_ci need tables of their own).
const Collation* collation_by_id(uint16_t id);

}  // namespace kvasir::exec
