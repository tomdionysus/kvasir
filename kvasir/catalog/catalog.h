//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "kvasir/catalog/table.h"

// The catalog: the databases and tables a node holds, so far in memory on one
// node (step 2). In the cluster it becomes agreed state with a version (the
// design's "DDL"); its users see only this interface. Database and table
// names are compared as bytes, case and all, as MySQL compares them with
// lower_case_table_names = 0 (observed on the reference server). The
// SQL-level checks (a name's length and form, a table's definition) are the
// session's.
//
// Thread safety: every operation takes the catalog's one lock, held only for
// the operation; safe from any thread. No operation waits on anything else.
namespace kvasir::catalog {

struct Database {
  std::string name;
  // The defaults its tables take.
  std::string charset;
  std::string collation;
};

struct TableName {
  std::string database;
  std::string name;
  friend bool operator==(const TableName&, const TableName&) = default;
  friend auto operator<=>(const TableName&, const TableName&) = default;
};

class Catalog {
 public:
  // False, and nothing changed, when a database of that name exists.
  bool create_database(Database database);
  // False when none of that name exists. Its tables go with it; `tables`,
  // when given, is set to how many.
  bool drop_database(const std::string& name, size_t* tables = nullptr);
  // A read that finds nothing succeeds with nothing.
  std::optional<Database> database(const std::string& name) const;
  // Every database's name, in byte order.
  std::vector<std::string> database_names() const;

  enum class Created : uint8_t { created, exists, no_database };
  // Adds the table to the database, giving it its id; nothing changes unless
  // it is created.
  Created create_table(const std::string& database, Table table);
  // Drops the tables named, all or none: the names not found, in the order
  // given. With `if_exists` the ones found are dropped all the same;
  // without, nothing is dropped when any is missing.
  std::vector<TableName> drop_tables(const std::vector<TableName>& names, bool if_exists);
  std::optional<Table> table(const TableName& name) const;
  // The database's tables' names in byte order; nothing when there is no
  // such database.
  std::optional<std::vector<std::string>> table_names(const std::string& database) const;

 private:
  mutable dannan::contract::Mutex _mutex;
  std::map<std::string, Database> _databases DANNAN_GUARDED_BY(_mutex);
  std::map<TableName, Table> _tables DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_table_id DANNAN_GUARDED_BY(_mutex) = 1;
};

}  // namespace kvasir::catalog
