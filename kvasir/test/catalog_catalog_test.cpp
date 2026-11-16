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

}  // namespace
}  // namespace kvasir::catalog
