//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/catalog/catalog.h"

namespace kvasir::catalog {

bool Catalog::create_database(Database database) {
  dannan::contract::Lock lock(_mutex);
  const auto name = database.name;
  return _databases.emplace(name, std::move(database)).second;
}

bool Catalog::drop_database(const std::string& name) {
  dannan::contract::Lock lock(_mutex);
  return _databases.erase(name) > 0;
}

std::optional<Database> Catalog::database(const std::string& name) const {
  dannan::contract::Lock lock(_mutex);
  const auto it = _databases.find(name);
  if (it == _databases.end()) return std::nullopt;
  return it->second;
}

std::vector<std::string> Catalog::database_names() const {
  dannan::contract::Lock lock(_mutex);
  std::vector<std::string> names;
  for (const auto& [name, unused] : _databases) names.push_back(name);
  return names;
}

}  // namespace kvasir::catalog
