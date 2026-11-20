//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The key encoding and ORDER BY comparison as their contracts in rows.h
// state them: a NULL key part sorts first and is named NULL; ORDER BY puts
// NULL first, numbers by value whatever their kind, strings under the
// collation.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "kvasir/exec/decimal.h"
#include "kvasir/exec/rows.h"

namespace kvasir::exec {
namespace {

catalog::Table two_columns() {
  catalog::Table t;
  t.name = "t";
  catalog::Column a;
  a.name = "a";
  catalog::Column b;
  b.name = "b";
  b.type.kind = catalog::TypeKind::varchar;
  b.type.length = 10;
  b.type.charset = "utf8mb4";
  b.type.collation = "utf8mb4_0900_ai_ci";
  t.columns = {a, b};
  catalog::Key k;
  k.kind = catalog::Key::Kind::index;
  k.name = "k";
  k.parts = {{0}, {1}};
  t.keys = {k};
  return t;
}

TEST(ExecRows, ANullKeyPartSortsFirst) {
  const auto t = two_columns();
  const txn::Row null_a{std::nullopt, std::string("x")};
  const txn::Row low_a{std::string("-5"), std::string("x")};
  const auto null_key = encode_key(t, t.keys[0], null_a);
  EXPECT_LT(null_key, encode_key(t, t.keys[0], low_a));
  EXPECT_EQ(null_key[0], '\0');
  EXPECT_LT(encode_key(t, t.keys[0], {std::string("1"), std::nullopt}), encode_key(t, t.keys[0], {std::string("1"), std::string("")}));
}

TEST(ExecRows, ANullKeyPartIsNamedNull) {
  const auto t = two_columns();
  EXPECT_EQ(key_text(t, t.keys[0], {std::nullopt, std::string("x")}), "NULL-x");
  EXPECT_EQ(key_text(t, t.keys[0], {std::string("3"), std::nullopt}), "3-NULL");
}

TEST(ExecRows, NumbersOfEveryKindOrderByValue) {
  EXPECT_EQ(compare_for_order(Value{1.5}, Value{int64_t{2}}, 0), -1);
  EXPECT_EQ(compare_for_order(Value{uint64_t{3}}, Value{2.5}, 0), 1);
  EXPECT_EQ(compare_for_order(Value{Decimal::from_int(2)}, Value{2.0}, 0), 0);
  EXPECT_EQ(compare_for_order(Value{int64_t{-1}}, Value{uint64_t{0}}, 0), -1);
  // A string against a number, as the number it reads as.
  EXPECT_EQ(compare_for_order(Value{std::string("10")}, Value{9.5}, 0), 1);
  EXPECT_EQ(compare_for_order(Value{2.0}, Value{std::string("2abc")}, 0), 0);
  EXPECT_EQ(compare_for_order(Value{}, Value{1.0}, 0), -1);
  EXPECT_EQ(compare_for_order(Value{}, Value{}, 0), 0);
}

}  // namespace
}  // namespace kvasir::exec
