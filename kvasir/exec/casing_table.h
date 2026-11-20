//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>

// The case tables kvasir-casing compiles at build time from
// kvasir/data/unicode/: each code point whose simple uppercase or lowercase
// differs from it, in code point order. casing.h is their user.
namespace kvasir::exec::casing {

struct CasePair {
  char32_t code;
  char32_t upper;
  char32_t lower;
};

extern const CasePair unicode9[];
extern const size_t unicode9_size;
extern const CasePair unicode3[];
extern const size_t unicode3_size;

}  // namespace kvasir::exec::casing
