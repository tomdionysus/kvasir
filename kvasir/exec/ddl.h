//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <string>
#include <vector>

#include "kvasir/catalog/table.h"
#include "kvasir/exec/expression.h"
#include "kvasir/sql/ast.h"

// CREATE TABLE's checks and what a table's columns report, as MySQL 8.4.13
// makes and reports them (observed 2026-11-16): the data types with their
// limits and MySQL's deprecation warnings, NULL and DEFAULT, AUTO_INCREMENT,
// keys and their names, the 3,072-byte key and 65,535-byte row limits, and
// the engine. Text columns take utf8mb4, utf8mb3 or binary; the rest of
// MySQL's character sets are refused as not yet converted.
namespace kvasir::exec {

struct TableContext {
  // Where it goes, as named in errors and warnings.
  std::string database;
  // The database's defaults.
  std::string charset;
  std::string collation;
  // sql_mode's NO_ENGINE_SUBSTITUTION.
  bool no_engine_substitution = true;
  // For DEFAULT's literals.
  EvalSettings eval;
};

// A table's name as CREATE TABLE checks it: 1059 past 64 characters, 1103
// for an empty one or one ending in a space.
void check_table_name(const std::string& name);

// The warnings MySQL gives as it reads the column types (1681), which come
// whether or not the table is then made.
void type_warnings(const sql::CreateTable& spec, std::vector<Warning>& warnings);

// The table the definition makes, its warnings and notes added. Throws
// SqlError for a definition MySQL refuses.
catalog::Table define_table(const sql::CreateTable& spec, const TableContext& context, std::vector<Warning>& warnings);

// What a table's column reports as a result column, its keys in its flags:
// text in the results' collation, at `result_max_bytes` a character.
ResultType column_result_type(const catalog::Table& table, size_t column, const EvalSettings& eval);

// The id of a column's own collation (0 for a column that is not text).
uint16_t column_collation_id(const catalog::Column& column);

}  // namespace kvasir::exec
