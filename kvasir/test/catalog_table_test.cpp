//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// What a column type takes in a row and in a key, and which key serves as the
// primary key. Observed on MySQL 8.4.13 (2026-11-16): the 65,535-byte row
// limit falls exactly where these lengths, plus a byte per eight nullable
// columns, reach it (VARBINARY(65470) beside TEXT, LONGBLOB, DECIMAL(20,5),
// BINARY(10), TINYBLOB and MEDIUMTEXT is created, VARBINARY(65471) is
// refused); the 3,072-byte key limit takes VARCHAR(768) in utf8mb4 and
// refuses VARCHAR(1000); PRI_KEY marks the first UNIQUE key of NOT NULL
// columns when there is no PRIMARY KEY.

#include <gtest/gtest.h>

#include "kvasir/catalog/table.h"

namespace kvasir::catalog {
namespace {

ColumnType type(TypeKind kind, uint32_t length = 0, uint8_t scale = 0, const std::string& charset = "") {
  ColumnType t;
  t.kind = kind;
  t.length = length;
  t.scale = scale;
  t.charset = charset;
  return t;
}

TEST(CatalogTable, PackLengths) {
  EXPECT_EQ(pack_length(type(TypeKind::tinyint)), 1u);
  EXPECT_EQ(pack_length(type(TypeKind::smallint)), 2u);
  EXPECT_EQ(pack_length(type(TypeKind::mediumint)), 3u);
  EXPECT_EQ(pack_length(type(TypeKind::int_, 5)), 4u);
  EXPECT_EQ(pack_length(type(TypeKind::bigint)), 8u);
  EXPECT_EQ(pack_length(type(TypeKind::float_)), 4u);
  EXPECT_EQ(pack_length(type(TypeKind::double_)), 8u);
  // Nine digits to four bytes, the rest by MySQL's table, either side of the
  // point.
  EXPECT_EQ(pack_length(type(TypeKind::decimal, 10, 0)), 5u);
  EXPECT_EQ(pack_length(type(TypeKind::decimal, 20, 5)), 10u);
  EXPECT_EQ(pack_length(type(TypeKind::decimal, 65, 30)), 30u);
  EXPECT_EQ(pack_length(type(TypeKind::decimal, 5, 2)), 3u);
  EXPECT_EQ(pack_length(type(TypeKind::char_, 10, 0, "utf8mb4")), 40u);
  EXPECT_EQ(pack_length(type(TypeKind::char_, 10, 0, "utf8mb3")), 30u);
  EXPECT_EQ(pack_length(type(TypeKind::binary, 10)), 10u);
  // A length byte up to 255 bytes, two past.
  EXPECT_EQ(pack_length(type(TypeKind::varbinary, 255)), 256u);
  EXPECT_EQ(pack_length(type(TypeKind::varbinary, 256)), 258u);
  EXPECT_EQ(pack_length(type(TypeKind::varbinary, 65532)), 65534u);
  EXPECT_EQ(pack_length(type(TypeKind::varchar, 63, 0, "utf8mb4")), 253u);
  EXPECT_EQ(pack_length(type(TypeKind::varchar, 64, 0, "utf8mb4")), 258u);
  EXPECT_EQ(pack_length(type(TypeKind::varchar, 16383, 0, "utf8mb4")), 65534u);
  EXPECT_EQ(pack_length(type(TypeKind::tinyblob)), 9u);
  EXPECT_EQ(pack_length(type(TypeKind::tinytext, 0, 0, "utf8mb4")), 9u);
  EXPECT_EQ(pack_length(type(TypeKind::text, 0, 0, "utf8mb4")), 10u);
  EXPECT_EQ(pack_length(type(TypeKind::mediumtext, 0, 0, "utf8mb4")), 11u);
  EXPECT_EQ(pack_length(type(TypeKind::longblob)), 12u);
}

TEST(CatalogTable, KeyLengths) {
  EXPECT_EQ(key_length(type(TypeKind::varchar, 768, 0, "utf8mb4"), 0), 3072u);
  EXPECT_EQ(key_length(type(TypeKind::varchar, 1000, 0, "utf8mb4"), 0), 4000u);
  EXPECT_EQ(key_length(type(TypeKind::varchar, 10, 0, "utf8mb4"), 3), 12u);
  EXPECT_EQ(key_length(type(TypeKind::text, 0, 0, "utf8mb4"), 100), 400u);
  EXPECT_EQ(key_length(type(TypeKind::varbinary, 100), 10), 10u);
  EXPECT_EQ(key_length(type(TypeKind::int_), 0), 4u);
  EXPECT_EQ(key_length(type(TypeKind::decimal, 20, 5), 0), 10u);
}

TEST(CatalogTable, TypeClasses) {
  EXPECT_TRUE(is_integer(TypeKind::mediumint));
  EXPECT_FALSE(is_integer(TypeKind::decimal));
  EXPECT_TRUE(is_numeric(TypeKind::double_));
  EXPECT_TRUE(is_text(TypeKind::varchar));
  EXPECT_TRUE(is_text(TypeKind::longtext));
  EXPECT_FALSE(is_text(TypeKind::blob));
  EXPECT_TRUE(is_binary(TypeKind::binary));
  EXPECT_TRUE(is_blob(TypeKind::mediumtext));
  EXPECT_FALSE(is_blob(TypeKind::varbinary));
  EXPECT_EQ(blob_max_bytes(TypeKind::tinytext), 255u);
  EXPECT_EQ(blob_max_bytes(TypeKind::blob), 65535u);
  EXPECT_EQ(blob_max_bytes(TypeKind::mediumblob), 16777215u);
  EXPECT_EQ(blob_max_bytes(TypeKind::longtext), 4294967295u);
  EXPECT_EQ(max_bytes_per_character("utf8mb4"), 4u);
  EXPECT_EQ(max_bytes_per_character("utf8mb3"), 3u);
  EXPECT_EQ(max_bytes_per_character("binary"), 1u);
}

Table with_columns(std::initializer_list<std::pair<const char*, bool>> columns) {
  Table t;
  for (const auto& [name, nullable] : columns) {
    Column c;
    c.name = name;
    c.nullable = nullable;
    t.columns.push_back(c);
  }
  return t;
}

Key key(Key::Kind kind, std::initializer_list<size_t> columns, uint32_t prefix = 0) {
  Key k;
  k.kind = kind;
  for (const auto c : columns) k.parts.push_back({c, prefix, false});
  return k;
}

TEST(CatalogTable, ThePrimaryKey) {
  auto t = with_columns({{"a", false}, {"B", true}, {"c", false}});
  EXPECT_EQ(t.column("b"), 1u);
  EXPECT_EQ(t.column("A"), 0u);
  EXPECT_FALSE(t.column("d"));
  EXPECT_FALSE(t.primary_key());
  // A UNIQUE key over a nullable column is not promoted; one over NOT NULL
  // columns is, the first such; PRIMARY KEY wins wherever it is written.
  t.keys.push_back(key(Key::Kind::unique, {1}));
  t.keys.push_back(key(Key::Kind::index, {0}));
  EXPECT_FALSE(t.primary_key());
  t.keys.push_back(key(Key::Kind::unique, {0, 2}));
  t.keys.push_back(key(Key::Kind::unique, {2}));
  EXPECT_EQ(t.primary_key(), 2u);
  t.keys.push_back(key(Key::Kind::primary, {2}));
  EXPECT_EQ(t.primary_key(), 4u);
  // A prefix is not the whole value: not promoted.
  auto p = with_columns({{"a", false}});
  p.keys.push_back(key(Key::Kind::unique, {0}, 3));
  EXPECT_FALSE(p.primary_key());
}

}  // namespace
}  // namespace kvasir::catalog
