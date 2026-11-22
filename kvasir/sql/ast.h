//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The syntax tree the parser builds: statements and their expressions, as
// written and not yet resolved against any catalog. One node type for
// expressions, its fields read by kind, so the tree is plain to walk and to
// print.
namespace kvasir::sql {

enum class ExprKind : uint8_t {
  null,             // NULL
  boolean,          // TRUE or FALSE; `value` is "TRUE" or "FALSE"
  integer,          // `value` as written
  decimal,          // `value` as written
  floating,         // `value` as written
  string,           // `value` unescaped and joined, `path` its parts (MySQL names a column by the first);
                    // `charset` an introducer's name, "national" for N''
  hex,              // `value` is the bytes (0x.., X'..')
  bits,             // `value` is the bytes (0b.., B'..')
  column,           // `path`: [schema.][table.]column
  user_variable,    // @name; `value` is the name
  system_variable,  // @@[scope.]name; `scope` upper case or empty, `value` the name
  placeholder,      // ?, in a prepared statement; `value` its number, from 0
  default_value,    // DEFAULT, in SET
  keyword_value,    // ON, ALL, BINARY, ROW or SYSTEM as SET's value; `value` upper case
  unary,            // `value`: "-", "~", "NOT", "BINARY"; args[0]
  binary,           // `value`: an operator below; args[0], args[1]
  is,               // args[0] IS [NOT] `value` (NULL, TRUE, FALSE, UNKNOWN)
  in,               // args[0] [NOT] IN (args[1..])
  between,          // args[0] [NOT] BETWEEN args[1] AND args[2]
  like,             // args[0] [NOT] LIKE args[1] [ESCAPE args[2]]
  regexp,           // args[0] [NOT] REGEXP args[1]
  call,             // `value` the function's name in upper case; args; `distinct`; `star` for COUNT(*);
                    // TRIM's args are str then remstr if any, `scope` BOTH, LEADING, TRAILING or empty
  case_when,        // `operand` if any; args in pairs, WHEN then THEN; `otherwise` if any
  row,              // (a, b, ...) or ROW(a, b, ...)
  collate,          // args[0] COLLATE `value`
  assign,           // args[0] (a user variable) := args[1]
};

// The binary operators, as `value` holds them: + - * / DIV % ^ & | << >>
// = <=> != < <= > >= AND OR XOR. Written forms are folded: && is AND, || is
// OR (or the CONCAT call under PIPES_AS_CONCAT), <> is !=, MOD is %, ! is NOT.

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::null;
  std::string value;
  std::string charset;
  std::string scope;
  std::vector<std::string> path;
  std::vector<ExprPtr> args;
  ExprPtr operand;
  ExprPtr otherwise;
  bool negated = false;
  bool distinct = false;
  bool star = false;
  // Where it begins in the statement's text.
  size_t offset = 0;
};

// An S-expression, for tests and logs: (+ 1 (* 2 3)), (IS_NOT a NULL),
// (COUNT DISTINCT a), @@SESSION.autocommit, 'it''s', X'0A'.
std::string to_string(const Expr& expr);

struct SelectItem {
  ExprPtr expr;
  // `*` or `t.*`: no expr, and `star_path` holds the qualifier.
  bool star = false;
  std::vector<std::string> star_path;
  bool has_alias = false;
  std::string alias;
  // The item as written: the column's name when it has no alias, as MySQL
  // names it.
  std::string text;
};

struct TableRef {
  std::vector<std::string> name;  // [schema.]table
  std::string alias;
};

struct OrderItem {
  ExprPtr expr;
  bool descending = false;
};

struct Select {
  std::string hint;
  bool distinct = false;
  std::vector<SelectItem> items;
  bool from_dual = false;
  std::vector<TableRef> from;
  ExprPtr where;
  // GROUP BY's items, as written: a column, an expression or a position.
  std::vector<ExprPtr> group_by;
  bool with_rollup = false;
  ExprPtr having;
  std::vector<OrderItem> order;
  ExprPtr limit;
  ExprPtr offset;
};

struct Assignment {
  enum class Target : uint8_t { user_variable, system_variable, names, character_set };
  Target target = Target::system_variable;
  // A system variable's scope as written (GLOBAL, SESSION, LOCAL, PERSIST,
  // PERSIST_ONLY), upper case, or empty for none.
  std::string scope;
  std::string name;
  ExprPtr value;
  // SET NAMES and SET CHARACTER SET: the character set and collation named,
  // or `use_default` for DEFAULT.
  std::string charset;
  std::string collation;
  bool use_default = false;
};

// A column's data type as written: `name` the type's canonical upper-case
// name (INT for INTEGER, DECIMAL for NUMERIC, DEC and FIXED, DOUBLE for REAL
// and DOUBLE PRECISION, CHAR for NCHAR, VARCHAR for NVARCHAR, BOOL for
// BOOLEAN), with the numbers and attributes given.
struct DataType {
  std::string name;
  std::optional<uint64_t> length;  // (M), or M of (M,D)
  std::optional<uint64_t> scale;   // D of (M,D)
  bool is_unsigned = false;
  bool zerofill = false;
  // CHARACTER SET, NATIONAL (utf8mb3), the BINARY attribute (the character
  // set's _bin collation) and COLLATE.
  std::string charset;
  bool binary_attribute = false;
  std::string collation;
};

struct ColumnSpec {
  std::string name;
  DataType type;
  // NULL or NOT NULL, the last written; nothing when neither is.
  std::optional<bool> nullable;
  // DEFAULT: a literal, possibly signed, or NULL.
  ExprPtr default_value;
  bool auto_increment = false;
  bool primary = false;  // PRIMARY KEY, or KEY
  bool unique = false;   // UNIQUE [KEY]
  std::string comment;
};

struct KeyPart {
  std::string column;
  std::optional<uint64_t> prefix;
  bool descending = false;
};

struct KeySpec {
  enum class Kind : uint8_t { primary, unique, index };
  Kind kind = Kind::index;
  // The index's name, or the CONSTRAINT's, or empty for one MySQL names.
  std::string name;
  std::vector<KeyPart> parts;
  std::string comment;
  // How many columns were defined before it: a column's own PRIMARY KEY or
  // UNIQUE comes in that order among the keys, as MySQL names them.
  size_t position = 0;
};

struct CreateTable {
  std::vector<std::string> name;  // [schema.]table
  bool if_not_exists = false;
  std::vector<ColumnSpec> columns;
  std::vector<KeySpec> keys;
  std::string engine;
  std::string charset;
  std::string collation;
  std::optional<uint64_t> auto_increment;
  std::string comment;
};

// INSERT [INTO] table [(columns)] VALUES (row), ... or INSERT ... SET
// column = value, ...: SET's columns are the column list and its values one
// row. DEFAULT in a row is ExprKind::default_value.
struct Insert {
  std::vector<std::string> table;  // [schema.]table
  std::vector<std::string> columns;
  // A column list was written, empty or not; without one every column is
  // given, in order.
  bool has_columns = false;
  std::vector<std::vector<ExprPtr>> rows;
  // INSERT IGNORE: a row's errors that can be ignored become warnings.
  bool ignore = false;
  // REPLACE: a row a key already holds is deleted for the new one.
  bool replace = false;
};

// UPDATE table [AS alias] SET column = value, ... and DELETE FROM table [AS
// alias], each with [WHERE] [ORDER BY] [LIMIT n]. DEFAULT as a value is
// ExprKind::default_value.
struct Change {
  TableRef table;
  // UPDATE's assignments: [table.]column and its value, in the order written.
  std::vector<std::pair<std::vector<std::string>, ExprPtr>> set;
  ExprPtr where;
  std::vector<OrderItem> order;
  ExprPtr limit;
};

struct Statement {
  enum class Kind : uint8_t {
    select,
    set,
    use,
    begin,
    commit,
    rollback,
    show_warnings,
    create_database,
    drop_database,
    show_databases,
    create_table,
    drop_table,
    show_tables,
    insert,
    update,
    delete_,
  };
  Kind kind = Kind::select;
  Select select;
  std::vector<Assignment> assignments;
  CreateTable create_table;
  Insert insert;
  Change change;
  // DROP TABLE's tables, each [schema.]table.
  std::vector<std::vector<std::string>> tables;
  // SHOW FULL TABLES.
  bool full = false;
  // Valid MySQL that Kvasir does not yet carry out, as its 1235 refusal names
  // it ("CREATE TABLE ... LIKE"); the rest of the statement is not read.
  std::string unsupported;
  // USE, CREATE DATABASE and DROP DATABASE: the database named; SHOW TABLES
  // FROM or IN: the one given, else empty.
  std::string database;
  // CREATE DATABASE IF NOT EXISTS, DROP DATABASE IF EXISTS, DROP TABLE IF
  // EXISTS.
  bool if_exists = false;
  // CREATE DATABASE's character set and collation, empty for none given.
  std::string charset;
  std::string collation;
  // SHOW DATABASES LIKE and SHOW TABLES LIKE: the pattern.
  std::optional<std::string> like;
  // START TRANSACTION READ ONLY.
  bool read_only = false;
  // COMMIT's optimizer hint, Kvasir's place for ack_level.
  std::string hint;
  // SHOW WARNINGS or SHOW ERRORS [LIMIT [offset,] count], or SHOW COUNT(*)
  // of either.
  bool errors_only = false;
  bool count_only = false;
  // Where it stands in the text sent.
  size_t offset = 0;
  size_t length = 0;
  // How many placeholders it holds (a prepared statement's parameters).
  size_t placeholders = 0;
};

}  // namespace kvasir::sql
