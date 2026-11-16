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

bool Catalog::drop_database(const std::string& name, size_t* tables) {
  dannan::contract::Lock lock(_mutex);
  if (_databases.erase(name) == 0) return false;
  size_t dropped = 0;
  for (auto it = _tables.lower_bound({name, ""}); it != _tables.end() && it->first.database == name; ++dropped) it = _tables.erase(it);
  if (tables) *tables = dropped;
  return true;
}

Catalog::Created Catalog::create_table(const std::string& database, Table table) {
  dannan::contract::Lock lock(_mutex);
  if (!_databases.contains(database)) return Created::no_database;
  TableName key{database, table.name};
  if (_tables.contains(key)) return Created::exists;
  table.id = _next_table_id++;
  _tables.emplace(std::move(key), std::move(table));
  return Created::created;
}

std::vector<TableName> Catalog::drop_tables(const std::vector<TableName>& names, bool if_exists) {
  dannan::contract::Lock lock(_mutex);
  std::vector<TableName> missing;
  for (const auto& name : names)
    if (!_tables.contains(name)) missing.push_back(name);
  if (missing.empty() || if_exists)
    for (const auto& name : names) _tables.erase(name);
  return missing;
}

std::optional<Table> Catalog::table(const TableName& name) const {
  dannan::contract::Lock lock(_mutex);
  const auto it = _tables.find(name);
  if (it == _tables.end()) return std::nullopt;
  return it->second;
}

std::optional<std::vector<std::string>> Catalog::table_names(const std::string& database) const {
  dannan::contract::Lock lock(_mutex);
  if (!_databases.contains(database)) return std::nullopt;
  std::vector<std::string> names;
  for (auto it = _tables.lower_bound({database, ""}); it != _tables.end() && it->first.database == database; ++it) names.push_back(it->first.name);
  return names;
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
