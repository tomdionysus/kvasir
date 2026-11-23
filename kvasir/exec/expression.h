//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
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
// An aggregate is read from an AggregateSource; anywhere else it is error
// 1111.
//
// The flow control functions (IFNULL, COALESCE, NULLIF, IF, CASE) type their
// result by aggregating their operands' types as MySQL does (observed).
//
// Not yet: REGEXP, functions other than the aggregates (aggregate.h), the
// flow control functions and the session's, each refused with its MySQL
// error. A placeholder is a prepared statement's bound parameter: its type
// and value are the binding's, and a string one compares as a literal does.
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
  // The column as an error message names it: by default its path as
  // written, each name backquoted (observed: a HAVING alias is `x`).
  virtual std::string written(const std::vector<std::string>& path) const;
  // VALUES(column) in ON DUPLICATE KEY UPDATE: the column of the row being
  // inserted, and its value; nothing anywhere else.
  virtual std::optional<Found> inserted_column(const std::string&) const { return std::nullopt; }
  virtual Value inserted_value(const std::string&) const { return {}; }
};

// Where aggregates are read from: the group at hand's, each call by its
// node in the statement.
class AggregateSource {
 public:
  virtual ~AggregateSource() = default;
  // The call's type and its value over the group, or nothing for a call it
  // did not collect.
  virtual std::optional<ResultType> aggregate_type(const sql::Expr& call) const = 0;
  virtual Value aggregate_value(const sql::Expr& call) const = 0;
};

// Whether a call's name is an aggregate (the manual's "Aggregate Function
// Descriptions").
bool is_aggregate(const sql::Expr& e);

// A prepared statement's parameter as bound: the type it is read as, and its
// value.
struct Parameter {
  ResultType type;
  Value value;
};

struct EvalSettings {
  // A prepared statement's bound parameters, by placeholder number; without
  // them a placeholder is not supported.
  const std::vector<Parameter>* parameters = nullptr;
  // Variables; without them a variable is not supported.
  const VariableSource* variables = nullptr;
  // A table's columns; without them a column name is unknown.
  const ColumnSource* columns = nullptr;
  // The group's aggregates; without them an aggregate is error 1111.
  const AggregateSource* aggregates = nullptr;
  // div_precision_increment.
  int div_precision_increment = 4;
  // The connection's collation: a string literal's own, and a number's as
  // text. Types are each expression's own; what is reported to the client
  // is `reported()`'s.
  uint16_t result_collation = 255;
  // Its widest character, in bytes.
  uint32_t result_max_bytes = 4;
  // Whether it is a _bin collation, whose string results MySQL flags BINARY
  // (observed).
  bool result_collation_binary = false;
  // The statement's time, microseconds since the Unix epoch in UTC: what
  // NOW() and its kin read, the same for the statement's length.
  int64_t statement_us = 0;
  // The session's time zone, as seconds east of UTC.
  int32_t zone_seconds = 0;
  // The time as it is now, for SYSDATE(); none reads the statement's.
  std::function<int64_t()> clock_us;
};

// A value as the bit operators and BIT_AND, BIT_OR and BIT_XOR take it, a
// negative as its two's complement (observed): a DECIMAL rounded half away
// from zero, a DOUBLE half to even, a string by its integer prefix, a hex or
// bit literal as its bytes, big-endian.
uint64_t bit_operand(const Value& v, const ResultType& from);

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

  // A value read as a DOUBLE: a string by its longest numeric prefix, with
  // warning 1292 when anything but trailing space is left.
  double real(const Value& v, const ResultType& t);
  // A value as a condition: nothing for NULL; a string read as real() reads
  // it.
  std::optional<bool> truth(const Value& v, const ResultType& t);

  const std::vector<Warning>& warnings() const noexcept { return _warnings; }
  void clear_warnings() { _warnings.clear(); }
  void add_warnings(const std::vector<Warning>& more) { _warnings.insert(_warnings.end(), more.begin(), more.end()); }

 private:
  struct Operand {
    const sql::Expr* expr;
    ResultType type;
  };
  using Derivation = std::pair<uint16_t, int>;
  // The collation operands compare under, aggregated as MySQL does, or
  // nothing when they compare as numbers. Throws SqlError: 1267 or 1270 for
  // collations that cannot be reconciled, 1235 for one Kvasir cannot yet
  // compare under. `operation` names the operation in 1267 and 1270.
  std::optional<uint16_t> string_collation(const std::vector<Operand>& operands, const std::string& operation) const;
  // Strings' collations aggregated (the lower coercibility wins; on a tie a
  // binary string, then a _bin collation), or 1267 or 1270.
  Derivation aggregate_collations(const std::vector<Derivation>& strings, const std::string& operation) const;
  // -1, 0 or 1, or nothing when either is NULL; two strings compare under
  // `collation`. A quiet operand read as a number gives no warning.
  std::optional<int> compare_values(const Value& x, const ResultType& tx, const Value& y, const ResultType& ty, std::optional<uint16_t> collation,
                                    bool quiet_x = false, bool quiet_y = false);

  // The flow control functions (IFNULL, COALESCE, NULLIF, IF and CASE):
  // the type several operands give together, NULL aside, and the collation
  // a string result holds.
  struct Merged {
    ResultType type;
    Derivation derivation;
  };
  Merged merged(const std::vector<Operand>& operands, const std::string& operation) const;
  Merged flow_type(const sql::Expr& e) const;
  // The collation a CASE's operand compares with its WHEN values under:
  // theirs aggregated when all are strings, else nothing (as numbers).
  std::optional<uint16_t> case_collation(const sql::Expr& e) const;
  Value flow_value(const sql::Expr& e);
  // A placeholder's bound parameter.
  const Parameter& parameter(const sql::Expr& e) const;
  // A value of one type as another's, as a merged result shows it.
  Value converted(const Value& v, const ResultType& from, const ResultType& to);

  // The string functions (functions.cpp): a call's type and the collation
  // its result holds, or nothing for a name that is not one of them.
  std::optional<Merged> string_function(const sql::Expr& e) const;
  Value string_function_value(const sql::Expr& e);
  // VALUES(column) as ON DUPLICATE KEY UPDATE reads it: the inserted row's
  // column, or nothing when `e` is not that or the statement has none.
  std::optional<ColumnSource::Found> inserted(const sql::Expr& e) const;
  // An argument's collation as a string function compares it: a string's
  // own, a number's the connection's at NUMERIC coercibility, NULL's binary
  // at IGNORABLE.
  Derivation text_derivation(const sql::Expr& e) const;
  // An argument read as an integer (a position, a length): a number
  // rounded, a string by its leading digits with warning 1292 for what is
  // left; nothing for NULL.
  std::optional<int64_t> integer_argument(const sql::Expr& e);
  // A constant argument read as an integer when typing (nothing inside for
  // NULL), or nothing for one that is not a constant.
  std::optional<std::optional<int64_t>> constant_integer(const sql::Expr& e) const;

  // The mathematical functions (functions.cpp): ABS, SIGN, FLOOR, CEILING,
  // ROUND and TRUNCATE; nothing for a name that is not one of them.
  std::optional<ResultType> number_function(const sql::Expr& e) const;
  Value number_function_value(const sql::Expr& e);
  // CAST and CONVERT (cast.cpp): the type a call converts to (1425, 1426,
  // 1427 for a DECIMAL it cannot be), and its value converted.
  ResultType cast_type(const sql::Expr& e) const;
  Value cast_value(const sql::Expr& e);
  // NOW() and its kin (cast.cpp): their type, or nothing for a name that is
  // not one of them; and their value from the statement's time.
  std::optional<ResultType> time_function(const sql::Expr& e) const;
  Value time_function_value(const sql::Expr& e);
  // Two values one of which is temporal (cast.cpp): as moments when both
  // are DATEs and DATETIMEs or both TIMEs; a DATE or DATETIME beside a
  // string with the string read as one (1525 when it cannot be); a TIME
  // beside a string as strings; nothing when they compare as numbers.
  std::optional<int> compare_temporal(const Value& x, const ResultType& tx, const Value& y, const ResultType& ty);
  // Two values as numbers, a temporal one as the number it is.
  std::optional<int> compare_as_numbers(const Value& x, const ResultType& tx, const Value& y, const ResultType& ty, bool quiet_x, bool quiet_y);
  // A string operand's collation and coercibility.
  std::pair<uint16_t, int> derivation(const sql::Expr& e, const ResultType& t) const;
  // The character set an operand's value is in.
  std::string charset_of(const sql::Expr& e, const ResultType& t) const;
  // A COLLATE clause's collation, checked against its operand's character
  // set (1253, 1273).
  uint16_t explicit_collation(const sql::Expr& collate) const;

  EvalSettings _settings;
  std::vector<Warning> _warnings;
  // The literals a comparison has read as numbers: MySQL converts a
  // constant operand once for the statement, so warns for it once.
  std::set<const sql::Expr*> _converted;
};

// Computed result types, as MySQL reports them: a BIGINT of `precision`
// digits, a DECIMAL, a DOUBLE of `length`.
ResultType integer_type(int precision, bool is_unsigned, bool not_null);
ResultType decimal_type(int precision, int decimals, bool not_null);
ResultType double_type(uint32_t length, bool not_null);
// Whether a type is a string's (any character set, binary included).
bool is_string_type(const ResultType& t);
// Whether a collation is utf8mb4_bin or utf8mb4_0900_bin, whose string
// results MySQL flags BINARY.
bool is_bin_collation(uint16_t collation);

// How strongly an operand holds its collation (the manual's "Collation
// Coercibility in Expressions"): the lower wins.
inline constexpr int explicit_coercibility = 0, implicit_coercibility = 2, sysconst_coercibility = 3, coercible_coercibility = 4, numeric_coercibility = 5,
                     ignorable_coercibility = 6;
// A table column's type as an expression over it reports it (MIN, MAX,
// NULLIF, IFNULL): an integer as long as its type's digits, a TEXT's length
// counted in characters once more, and no key, ZEROFILL, BLOB or NOT NULL
// flags.
ResultType column_as_expression(const ResultType& column);

// A DATE, TIME or DATETIME type (cast.cpp).
bool is_temporal_type(const ResultType& t);
// A temporal type's value as the number it is in a numeric context:
// 20240229, 123456, 20240229123456 (a DECIMAL with its fraction); any
// other value as it is.
Value temporal_number(const Value& v, const ResultType& t);

// The widest character of a collation's character set, in bytes.
uint32_t widest_character(uint16_t collation);

// What a result column reports (observed on MySQL 8.4.13): a string is
// converted from its own type to `results`, the collation of the client's
// character_set_results, its length its characters times the results'
// widest character (a BLOB's length times it); a binary string is never
// converted, and nothing is when character_set_results is NULL (no
// `results`). Anything else is as it is.
ResultType reported(const ResultType& type, std::optional<uint16_t> results);

// The expression as MySQL writes it in an error message:
// (9223372036854775807 + 1), (-(9223372036854775808) - 1); a column as
// `columns` writes it.
std::string mysql_text(const sql::Expr& expr, const ColumnSource* columns = nullptr);

// A result column's name, as MySQL gives it: the alias; else a string
// literal's value; else the item as written. Leading blanks and control
// characters go; a generated name keeps at most 255 bytes, an alias 256, a
// character whole (observed).
std::string column_name(const sql::SelectItem& item);
// An alias as a result column's name holds it.
std::string alias_name(const std::string& alias);
// What MySQL warns of an alias it changed: 1466 (with its unchanged text) or,
// when nothing is left, 1474; nothing for one kept whole.
std::optional<Warning> alias_warning(const std::string& alias);

}  // namespace kvasir::exec
