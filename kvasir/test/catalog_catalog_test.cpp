//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/catalog/catalog.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace kvasir::catalog {
namespace {

TEST(CatalogCatalog, DatabasesAreCreatedOnceAndDropped) {
  Catalog c;
  EXPECT_TRUE(c.database_names().empty());
  EXPECT_FALSE(c.database("d"));
  EXPECT_TRUE(c.create_database({"d", "utf8mb4", "utf8mb4_bin"}));
  EXPECT_FALSE(c.create_database({"d", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  ASSERT_TRUE(c.database("d"));
  // The second create changed nothing.
  EXPECT_EQ(c.database("d")->collation, "utf8mb4_bin");
  EXPECT_TRUE(c.drop_database("d"));
  EXPECT_FALSE(c.drop_database("d"));
  EXPECT_FALSE(c.database("d"));
}

// lower_case_table_names = 0: names are bytes, case and all (observed).
TEST(CatalogCatalog, NamesAreComparedAsBytesInByteOrder) {
  Catalog c;
  EXPECT_TRUE(c.create_database({"kv_probe", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  EXPECT_TRUE(c.create_database({"kv_Probe", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  EXPECT_TRUE(c.create_database({"kv.probe", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  EXPECT_FALSE(c.database("KV_PROBE"));
  EXPECT_EQ(c.database_names(), (std::vector<std::string>{"kv.probe", "kv_Probe", "kv_probe"}));
}

TEST(CatalogCatalog, ConcurrentCreatesOfOneNameMakeOne) {
  Catalog c;
  std::vector<std::thread> threads;
  std::vector<int> made(8, 0);
  for (size_t i = 0; i < made.size(); ++i) threads.emplace_back([&, i] { made[i] = c.create_database({"d", "utf8mb4", "utf8mb4_0900_ai_ci"}) ? 1 : 0; });
  for (auto& t : threads) t.join();
  int total = 0;
  for (const int m : made) total += m;
  EXPECT_EQ(total, 1);
  EXPECT_EQ(c.database_names().size(), 1u);
}

Table table(const std::string& name) {
  Table t;
  t.name = name;
  t.columns.push_back({});
  t.columns[0].name = "a";
  return t;
}

TEST(CatalogCatalog, TablesLiveInTheirDatabase) {
  Catalog c;
  EXPECT_EQ(c.create_table("d", table("t")), Catalog::Created::no_database);
  EXPECT_FALSE(c.table_names("d"));
  ASSERT_TRUE(c.create_database({"d", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  EXPECT_EQ(c.table_names("d"), std::vector<std::string>{});
  EXPECT_EQ(c.create_table("d", table("t")), Catalog::Created::created);
  auto again = table("t");
  again.comment = "second";
  EXPECT_EQ(c.create_table("d", again), Catalog::Created::exists);
  EXPECT_EQ(c.table({"d", "t"})->comment, "");
  // Bytes, case and all, in byte order.
  EXPECT_EQ(c.create_table("d", table("T")), Catalog::Created::created);
  EXPECT_EQ(c.create_table("d", table("t1")), Catalog::Created::created);
  EXPECT_EQ(c.table_names("d"), (std::vector<std::string>{"T", "t", "t1"}));
  EXPECT_FALSE(c.table({"d", "T1"}));
  // Every table has its own id.
  EXPECT_NE(c.table({"d", "t"})->id, 0u);
  EXPECT_NE(c.table({"d", "t"})->id, c.table({"d", "T"})->id);
}

// DROP TABLE drops all or none (observed: DROP TABLE t2, nothere, t3 leaves
// t2 and t3); with IF EXISTS the ones there go.
TEST(CatalogCatalog, DropTablesIsAllOrNone) {
  Catalog c;
  ASSERT_TRUE(c.create_database({"d", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  for (const char* name : {"t2", "t3", "t4"}) ASSERT_EQ(c.create_table("d", table(name)), Catalog::Created::created);
  const auto first = c.table({"d", "t2"})->id;
  EXPECT_EQ(c.drop_tables({{"d", "t2"}, {"d", "nothere"}, {"d", "t3"}}, false), (std::vector<TableName>{{"d", "nothere"}}));
  EXPECT_EQ(c.table_names("d"), (std::vector<std::string>{"t2", "t3", "t4"}));
  EXPECT_EQ(c.drop_tables({{"d", "t4"}, {"d", "nothere2"}}, true), (std::vector<TableName>{{"d", "nothere2"}}));
  EXPECT_EQ(c.table_names("d"), (std::vector<std::string>{"t2", "t3"}));
  EXPECT_TRUE(c.drop_tables({{"d", "t2"}}, false).empty());
  EXPECT_EQ(c.create_table("d", table("t2")), Catalog::Created::created);
  EXPECT_NE(c.table({"d", "t2"})->id, first);
  EXPECT_EQ(c.drop_tables({{"nodb", "t"}}, false), (std::vector<TableName>{{"nodb", "t"}}));
}

TEST(CatalogCatalog, ADatabaseTakesItsTablesWithIt) {
  Catalog c;
  ASSERT_TRUE(c.create_database({"d", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  ASSERT_TRUE(c.create_database({"e", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  ASSERT_EQ(c.create_table("d", table("t")), Catalog::Created::created);
  ASSERT_EQ(c.create_table("d", table("u")), Catalog::Created::created);
  ASSERT_EQ(c.create_table("e", table("t")), Catalog::Created::created);
  size_t dropped = 0;
  EXPECT_TRUE(c.drop_database("d", &dropped));
  EXPECT_EQ(dropped, 2u);
  EXPECT_FALSE(c.table({"d", "t"}));
  EXPECT_TRUE(c.table({"e", "t"}));
  ASSERT_TRUE(c.create_database({"d", "utf8mb4", "utf8mb4_0900_ai_ci"}));
  EXPECT_EQ(c.table_names("d"), std::vector<std::string>{});
}

}  // namespace
}  // namespace kvasir::catalog
