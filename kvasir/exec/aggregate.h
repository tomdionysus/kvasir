//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include "kvasir/exec/expression.h"
#include "kvasir/exec/value.h"
#include "kvasir/sql/ast.h"

// The aggregate functions of the manual's "Aggregate Function Descriptions":
// COUNT, SUM, AVG, MIN, MAX (each with DISTINCT), BIT_AND, BIT_OR, BIT_XOR,
// and STD, STDDEV, STDDEV_POP, STDDEV_SAMP, VARIANCE, VAR_POP and VAR_SAMP.
// NULL is skipped; over no rows COUNT and the BIT_ functions have a value
// and the rest are NULL. Result types as observed on MySQL 8.4.13:
// - COUNT: BIGINT, 21 long, NOT NULL.
// - SUM of an exact value: DECIMAL of 22 more digits at its scale; AVG of
//   one: DECIMAL of div_precision_increment more digits and decimals. Of a
//   DOUBLE or a string, either is DOUBLE (a string read as a number, warning
//   1292 for one that is not).
// - MIN and MAX: the argument's type, nullable; of a table's column, as an
//   expression reports it (no ZEROFILL, keys or BLOB flag; an integer at its
//   type's own digits; a string with 31 decimals, a TEXT's length counted in
//   characters once more).
// - BIT_AND, BIT_OR, BIT_XOR: unsigned BIGINT, NOT NULL; the rest DOUBLE.
// GROUP_CONCAT and the JSON aggregates are not yet supported (1235).
namespace kvasir::exec {

// One aggregate call of a statement, typed.
class AggregateSpec {
 public:
  // `args` are the argument's types; `column` whether the one argument is a
  // table's column; `collations` what each argument compares under (0 for
  // a number). Throws SqlError for a function not yet supported.
  AggregateSpec(const sql::Expr& call, std::vector<ResultType> args, std::vector<uint16_t> collations, bool column, const EvalSettings& settings);

  enum class Kind : uint8_t { count, sum, avg, min, max, bit_and, bit_or, bit_xor, var_pop, var_samp, std_pop, std_samp };

  const sql::Expr& call() const noexcept { return *_call; }
  Kind kind() const noexcept { return _kind; }
  const ResultType& type() const noexcept { return _type; }
  const std::vector<ResultType>& args() const noexcept { return _args; }
  const std::vector<uint16_t>& collations() const noexcept { return _collations; }
  int div_precision_increment() const noexcept { return _increment; }

 private:
  const sql::Expr* _call;
  Kind _kind = Kind::count;
  std::vector<ResultType> _args;
  std::vector<uint16_t> _collations;
  ResultType _type;
  int _increment = 4;
};

// One aggregate call's running value over one group.
class Accumulator {
 public:
  explicit Accumulator(const AggregateSpec& spec);

  // A row: its argument values (none for COUNT(*)). `evaluator` reads a
  // string as a number, and keeps the warnings that gives.
  void add(const std::vector<Value>& args, Evaluator& evaluator);
  Value result() const;

 private:
  struct TupleLess {
    const std::vector<uint16_t>* collations;
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const;
  };

  const AggregateSpec* _spec;
  uint64_t _count = 0;
  Decimal _exact;
  double _real = 0;
  // Welford's running mean and sum of squares, as MySQL keeps them.
  double _mean = 0;
  double _m2 = 0;
  uint64_t _bits = 0;
  Value _best;
  std::set<std::vector<Value>, TupleLess> _seen;
};

}  // namespace kvasir::exec
