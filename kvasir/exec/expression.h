//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kvasir/exec/value.h"
#include "kvasir/sql/ast.h"

// Typing and evaluating expressions with no table under them: literals,
// arithmetic, comparisons, logic and bit operations, with MySQL 8.4's result
// types and its rules for NULL, overflow and division by zero, as observed on
// MySQL 8.4.13.
//
// Strings compare under the collation MySQL derives for the pair (the
// manual's "Collation Coercibility in Expressions", as observed): a literal
// has collation_connection's at coercibility 4, a user variable the one it
// was set with at 2, a system variable or session function
// utf8mb3_general_ci at 3, a COLLATE clause its own at 0. The lower
// coercibility wins; on a tie a binary string wins, then a _bin collation
// beside another (never between two COLLATE clauses), and anything else is
// error 1267 (1270 for IN and BETWEEN). A collation Kvasir cannot yet
// compare under (collation.h) is refused.
//
// LIKE matches under the same collation (Collation::like).
//
// A table's column is read through a ColumnSource and holds its own
// collation at coercibility 2.
//
// Not yet: REGEXP, functions and placeholders, each refused with its MySQL
// error.
// Known difference: arithmetic on a bare NULL literal is typed DOUBLE with
// length 23 and 31 decimals, where MySQL reports smaller lengths; its value
// is NULL either way.
namespace kvasir::exec {

// Where variables are read from: a session's.
class VariableSource {
 public:
  virtual ~VariableSource() = default;
  // @@[scope.]name. Each throws SqlError for a name or scope not known.
  virtual ResultType system_type(const std::string& scope, const std::string& name) const = 0;
  virtual Value system_value(const std::string& scope, const std::string& name) const = 0;
  // @name: NULL with its type when never set.
  virtual ResultType user_type(const std::string& name) const = 0;
  virtual Value user_value(const std::string& name) const = 0;
  // A function of the session with no arguments (DATABASE(), USER(),
  // CONNECTION_ID(), ...): its type and value, or false for a name it does
  // not have.
  virtual bool session_function(const std::string& name, ResultType& type, Value& value) const = 0;
};

// Where columns are read from: the row of the table a statement runs over.
class ColumnSource {
 public:
  virtual ~ColumnSource() = default;
  struct Found {
    // As a result column reports it.
    ResultType type;
    // The column's own collation, which it compares under at coercibility 2.
    uint16_t collation = 0;
  };
  // The column a [[schema.]table.]column path names, or nothing for none.
  virtual std::optional<Found> find_column(const std::vector<std::string>& path) const = 0;
  // Its value in the row at hand.
  virtual Value column_value(const std::vector<std::string>& path) const = 0;
};

struct EvalSettings {
  // Variables; without them a variable is not supported.
  const VariableSource* variables = nullptr;
  // A table's columns; without them a column name is unknown.
  const ColumnSource* columns = nullptr;
  // div_precision_increment.
  int div_precision_increment = 4;
  // The collation a string result is reported in: character_set_results'.
  uint16_t result_collation = 255;
  // Its widest character, in bytes.
  uint32_t result_max_bytes = 4;
  // Whether it is a _bin collation, whose string results MySQL flags BINARY
  // (observed).
  bool result_collation_binary = false;
};

class Evaluator {
 public:
  explicit Evaluator(EvalSettings settings = {});

  // The column an expression's value fills. Throws SqlError for what cannot
  // be typed (not yet supported, unknown columns).
  ResultType type(const sql::Expr& expr) const;
  // Its value. Throws SqlError (an overflow, as 1690); a division by zero is
  // NULL with warning 1365, and a string read as a number with text left over
  // gives warning 1292.
  Value value(const sql::Expr& expr);

  const std::vector<Warning>& warnings() const noexcept { return _warnings; }
  void clear_warnings() { _warnings.clear(); }

 private:
  struct Operand {
    const sql::Expr* expr;
    ResultType type;
  };
  // The collation operands compare under, aggregated as MySQL does, or
  // nothing when they compare as numbers. Throws SqlError: 1267 or 1270 for
  // collations that cannot be reconciled, 1235 for one Kvasir cannot yet
  // compare under. `operation` names the operation in 1267 and 1270.
  std::optional<uint16_t> string_collation(const std::vector<Operand>& operands, const std::string& operation) const;
  // A string operand's collation and coercibility.
  std::pair<uint16_t, int> derivation(const sql::Expr& e, const ResultType& t) const;
  // The character set an operand's value is in.
  std::string charset_of(const sql::Expr& e, const ResultType& t) const;
  // A COLLATE clause's collation, checked against its operand's character
  // set (1253, 1273).
  uint16_t explicit_collation(const sql::Expr& collate) const;

  EvalSettings _settings;
  std::vector<Warning> _warnings;
};

// The expression as MySQL writes it in an error message:
// (9223372036854775807 + 1), (-(9223372036854775808) - 1).
std::string mysql_text(const sql::Expr& expr);

// A result column's name, as MySQL gives it: the alias; else a string
// literal's value; else the item as written.
std::string column_name(const sql::SelectItem& item);

}  // namespace kvasir::exec
