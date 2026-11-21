//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Each fuzz target (kvasir/fuzz/) through Dannan's smoke run in the test
// binary on every gate, with no clang: its seeds, mutations of them, random
// inputs and the empty input. A target throwing anything is a finding.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "dannan/fuzz/fuzz.h"

namespace kvasir {
namespace {

TEST(FuzzSmoke, EveryTargetRunsClean) {
  std::set<std::string> names;
  for (const auto& target : dannan::fuzz::targets()) {
    names.insert(target.name);
    size_t ran = 0;
    EXPECT_NO_THROW(ran = dannan::fuzz::smoke(target)) << target.name;
    EXPECT_GT(ran, 3000u) << target.name;
  }
  // Every parser of untrusted bytes has its target here.
  EXPECT_EQ(names, (std::set<std::string>{"execute", "lexer", "packet", "parser"}));
}

}  // namespace
}  // namespace kvasir
