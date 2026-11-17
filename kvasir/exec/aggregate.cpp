//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/aggregate.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "kvasir/exec/rows.h"
#include "kvasir/protocol/types.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;
using Kind = AggregateSpec::Kind;

[[noreturn]] void not_yet(const std::string& what) {
  throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support '" + what + "'");
}

bool is_integer_type(const ResultType& t) {
  return t.field_type == f::longlong || t.field_type == f::long_ || t.field_type == f::int24 || t.field_type == f::short_ || t.field_type == f::tiny ||
         t.number_string;
}

// The digits an integer of this type can hold.
int integer_digits(const ResultType& t) {
  switch (t.field_type) {
    case f::tiny:
      return 3;
    case f::short_:
      return 5;
    case f::int24:
      return 8;
    case f::long_:
      return 10;
    default:
      return t.is_unsigned() ? 20 : 19;
  }
}

Kind kind_of(const std::string& name) {
  if (name == "COUNT") return Kind::count;
  if (name == "SUM") return Kind::sum;
  if (name == "AVG") return Kind::avg;
  if (name == "MIN") return Kind::min;
  if (name == "MAX") return Kind::max;
  if (name == "BIT_AND") return Kind::bit_and;
  if (name == "BIT_OR") return Kind::bit_or;
  if (name == "BIT_XOR") return Kind::bit_xor;
  if (name == "VARIANCE" || name == "VAR_POP") return Kind::var_pop;
  if (name == "VAR_SAMP") return Kind::var_samp;
  if (name == "STD" || name == "STDDEV" || name == "STDDEV_POP") return Kind::std_pop;
  if (name == "STDDEV_SAMP") return Kind::std_samp;
  not_yet(name);
}

// MIN or MAX of a table's column: reported as an expression over it would
// be, not as the column.
ResultType as_expression(const ResultType& column, uint32_t result_max_bytes) {
  ResultType t = column;
  const bool unsigned_ = column.is_unsigned();
  if (is_string_type(column)) {
    t.decimals = protocol::not_fixed_decimals;
    if (column.field_type == f::blob && column.collation != protocol::collation::binary)
      t.length = static_cast<uint32_t>(std::min<uint64_t>(uint64_t{column.length} * result_max_bytes, UINT32_MAX));
    t.flags = static_cast<uint16_t>(column.collation == protocol::collation::binary || (column.flags & c::binary) ? c::binary : 0);
    return t;
  }
  if (is_integer_type(column)) t.length = static_cast<uint32_t>(integer_digits(column) + (unsigned_ ? 0 : 1));
  t.flags = static_cast<uint16_t>(c::binary | c::num | (unsigned_ ? c::unsigned_ : 0));
  return t;
}

Decimal exact_of(const Value& v) {
  if (const auto* d = std::get_if<Decimal>(&v)) return *d;
  if (const auto* i = std::get_if<int64_t>(&v)) return Decimal::from_int(*i);
  if (const auto* u = std::get_if<uint64_t>(&v)) return Decimal::from_uint(*u);
  // A hex or bit literal: an unsigned integer, big-endian.
  uint64_t out = 0;
  if (const auto* s = std::get_if<std::string>(&v))
    for (const char ch : *s) out = (out << 8) | static_cast<uint8_t>(ch);
  return Decimal::from_uint(out);
}

}  // namespace

AggregateSpec::AggregateSpec(const sql::Expr& call, std::vector<ResultType> args, std::vector<uint16_t> collations, bool column, const EvalSettings& settings)
    : _call(&call), _kind(kind_of(call.value)), _args(std::move(args)), _collations(std::move(collations)), _increment(settings.div_precision_increment) {
  if (_kind != Kind::count && _args.size() != 1) not_yet(call.value + " of more than one value");
  if (call.distinct && _kind != Kind::count && _kind != Kind::sum && _kind != Kind::avg && _kind != Kind::min && _kind != Kind::max)
    not_yet(call.value + "(DISTINCT)");
  const ResultType* a = _args.empty() ? nullptr : &_args[0];
  const bool exact = a && a->field_type != f::null && (is_integer_type(*a) || a->field_type == f::newdecimal);
  const int scale = a && a->field_type == f::newdecimal ? a->decimals : 0;
  switch (_kind) {
    case Kind::count:
      _type = integer_type(20, false, true);
      break;
    case Kind::sum:
      _type = exact ? decimal_type(std::min(a->precision + 22, static_cast<int>(decimal_max_precision)), scale, false) : double_type(23, false);
      break;
    case Kind::avg:
      _type = exact ? decimal_type(std::min(a->precision + _increment, static_cast<int>(decimal_max_precision)),
                                   std::min(scale + _increment, static_cast<int>(decimal_max_scale)), false)
                    : double_type(23, false);
      break;
    case Kind::min:
    case Kind::max:
      _type = column ? as_expression(*a, settings.result_max_bytes) : *a;
      _type.flags = static_cast<uint16_t>(_type.flags & ~c::not_null);
      break;
    case Kind::bit_and:
    case Kind::bit_or:
    case Kind::bit_xor:
      _type = integer_type(21, true, true);
      break;
    default:
      _type = double_type(23, false);
  }
}

bool Accumulator::TupleLess::operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
  for (size_t i = 0; i < a.size(); ++i) {
    const int order = compare_for_order(a[i], b[i], (*collations)[i]);
    if (order != 0) return order < 0;
  }
  return false;
}

Accumulator::Accumulator(const AggregateSpec& spec) : _spec(&spec), _seen(TupleLess{&spec.collations()}) {}

void Accumulator::add(const std::vector<Value>& args, Evaluator& evaluator) {
  for (const auto& v : args)
    if (is_null(v)) return;
  if (_spec->call().distinct && !_seen.insert(args).second) return;
  ++_count;
  if (args.empty()) return;
  const auto& v = args[0];
  const bool exact = _spec->type().field_type == f::newdecimal;
  switch (_spec->kind()) {
    case Kind::count:
      break;
    case Kind::sum:
    case Kind::avg:
      if (exact)
        _exact = _exact + exact_of(v);
      else
        _real += evaluator.real(v, _spec->args()[0]);
      break;
    case Kind::min:
    case Kind::max: {
      if (is_null(_best)) {
        _best = v;
        break;
      }
      const int order = compare_for_order(v, _best, _spec->collations()[0]);
      if (_spec->kind() == Kind::min ? order < 0 : order > 0) _best = v;
      break;
    }
    case Kind::bit_and:
    case Kind::bit_or:
    case Kind::bit_xor: {
      uint64_t bits = 0;
      if (const auto* i = std::get_if<int64_t>(&v))
        bits = static_cast<uint64_t>(*i);
      else if (const auto* u = std::get_if<uint64_t>(&v))
        bits = *u;
      else
        bits = static_cast<uint64_t>(std::llround(evaluator.real(v, _spec->args()[0])));
      if (_spec->kind() == Kind::bit_and) _bits = _count == 1 ? bits : _bits & bits;
      if (_spec->kind() == Kind::bit_or) _bits |= bits;
      if (_spec->kind() == Kind::bit_xor) _bits ^= bits;
      break;
    }
    default: {
      const double x = evaluator.real(v, _spec->args()[0]);
      const double mean = _mean + (x - _mean) / static_cast<double>(_count);
      _m2 += (x - _mean) * (x - mean);
      _mean = mean;
    }
  }
}

Value Accumulator::result() const {
  const bool exact = _spec->type().field_type == f::newdecimal;
  switch (_spec->kind()) {
    case Kind::count:
      return static_cast<int64_t>(_count);
    case Kind::bit_and:
      return _count ? _bits : std::numeric_limits<uint64_t>::max();
    case Kind::bit_or:
    case Kind::bit_xor:
      return _bits;
    case Kind::min:
    case Kind::max:
      return _best;
    default:
      break;
  }
  if (_count == 0) return {};
  switch (_spec->kind()) {
    case Kind::sum:
      if (exact) return _exact;
      return _real;
    case Kind::avg:
      if (exact) return *Decimal::divide(_exact, Decimal::from_uint(_count), _spec->div_precision_increment());
      return _real / static_cast<double>(_count);
    case Kind::var_pop:
      return _m2 / static_cast<double>(_count);
    case Kind::std_pop:
      return std::sqrt(_m2 / static_cast<double>(_count));
    case Kind::var_samp:
      if (_count < 2) return {};
      return _m2 / static_cast<double>(_count - 1);
    default:
      if (_count < 2) return {};
      return std::sqrt(_m2 / static_cast<double>(_count - 1));
  }
}

}  // namespace kvasir::exec
