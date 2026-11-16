//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>

// Unicode's collation element table for UCA 9.0.0, as kvasir-ducet compiles
// it at build time from kvasir/data/unicode/allkeys-9.0.0.txt.
namespace kvasir::exec::ducet {

struct Element {
  uint16_t primary;
  uint16_t secondary;
  uint16_t tertiary;
};

// One code point, or a contraction of two or three (unused places zero),
// with its elements at [offset, offset + count). Sorted by its code points.
struct Entry {
  uint32_t first;
  uint32_t second;
  uint32_t third;
  uint32_t offset;
  uint16_t count;
};

extern const Element elements[];
extern const Entry entries[];
extern const size_t entry_count;

}  // namespace kvasir::exec::ducet
