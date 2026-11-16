//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"

// The catalog: the databases a node holds, so far in memory on one node (step
// 2). In the cluster it becomes agreed state with a version (the design's
// "DDL"); its users see only this interface. Names are compared as bytes,
// case and all, as MySQL compares them with lower_case_table_names = 0
// (observed on the reference server). The SQL-level checks (a name's length
// and form, a character set Kvasir holds) are the session's.
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

class Catalog {
 public:
  // False, and nothing changed, when a database of that name exists.
  bool create_database(Database database);
  // False when none of that name exists.
  bool drop_database(const std::string& name);
  // A read that finds nothing succeeds with nothing.
  std::optional<Database> database(const std::string& name) const;
  // Every database's name, in byte order.
  std::vector<std::string> database_names() const;

 private:
  mutable dannan::contract::Mutex _mutex;
  std::map<std::string, Database> _databases DANNAN_GUARDED_BY(_mutex);
};

}  // namespace kvasir::catalog
