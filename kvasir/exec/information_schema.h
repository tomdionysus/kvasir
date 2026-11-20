//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kvasir/catalog/catalog.h"
#include "kvasir/exec/expression.h"
#include "kvasir/txn/store.h"

// information_schema, read only, as MySQL 8.4.13 shows it (observed): the
// SCHEMATA, TABLES and COLUMNS views, built from the catalog when a statement
// names them. Each column has the type MySQL holds it in (names
// utf8mb3_bin, other text utf8mb3_general_ci, so a client is told them in
// its results' collation with their BINARY flags kept), and each value is
// the text MySQL shows. MySQL's other views are refused as not yet supported;
// TABLES' CREATE_TIME, UPDATE_TIME and CHECK_TIME are refused when named,
// until Kvasir has temporal types; its storage figures (TABLE_ROWS, the byte
// sizes, AUTO_INCREMENT, which MySQL takes from InnoDB's cached statistics)
// are NULL.
namespace kvasir::exec {

inline constexpr std::string_view information_schema_name = "information_schema";

// Whether a database name is information_schema's, in any case (observed).
bool is_information_schema(std::string_view database);

struct View {
  // Its name and columns: each column's ColumnType reads its text as a
  // value.
  catalog::Table table;
  // Each column's type as MySQL holds it, read directly, and read through
  // a temporary table (a statement with ORDER BY, GROUP BY or DISTINCT),
  // where some differ (observed).
  std::vector<ResultType> types;
  std::vector<ResultType> ordered;
  // Columns refused when a statement names them: the reason, or empty.
  std::vector<std::string> refused;
  std::vector<txn::Row> rows;
};

// The view `name` names (in any case), built from `catalog` now. Throws
// SqlError: 1109 for a name MySQL does not have, 1235 for one of MySQL's
// views Kvasir does not build yet.
View information_schema_view(const std::string& name, const catalog::Catalog& catalog);

}  // namespace kvasir::exec
