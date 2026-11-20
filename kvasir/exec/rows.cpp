//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/rows.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "kvasir/exec/collation.h"
#include "kvasir/exec/ddl.h"
#include "kvasir/exec/store.h"

namespace kvasir::exec {

namespace {

using catalog::TypeKind;

void append_big_endian(std::string& out, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xff));
}

// Bytes that sort as they are, ended so that a shorter value sorts first:
// each 0x00 becomes 0x00 0x01, and 0x00 0x00 ends it.
void append_terminated(std::string& out, const std::string& bytes) {
  for (const char ch : bytes) {
    out.push_back(ch);
    if (ch == '\0') out.push_back('\1');
  }
  out.push_back('\0');
  out.push_back('\0');
}

// Whether a collation pads with spaces: utf8mb4_bin, utf8mb3_bin and the
// general_ci collations.
bool pads(uint16_t collation) { return collation == 46 || collation == 83 || collation == 45 || collation == 33; }

size_t characters(const std::string& s) {
  size_t n = 0;
  for (const char ch : s)
    if ((static_cast<uint8_t>(ch) & 0xc0) != 0x80) ++n;
  return n;
}

int sign_of(double d) { return d < 0 ? -1 : d > 0 ? 1 : 0; }

}  // namespace

Value value_of(const catalog::ColumnType& type, const std::string& text) {
  if (catalog::is_integer(type.kind)) {
    if (type.is_unsigned) {
      uint64_t v = 0;
      std::from_chars(text.data(), text.data() + text.size(), v);
      return v;
    }
    int64_t v = 0;
    std::from_chars(text.data(), text.data() + text.size(), v);
    return v;
  }
  if (type.kind == TypeKind::decimal) return Decimal::parse(text).value_or(Decimal());
  if (type.kind == TypeKind::float_) return static_cast<double>(std::strtof(text.c_str(), nullptr));
  if (type.kind == TypeKind::double_) return std::strtod(text.c_str(), nullptr);
  return text;
}

std::string display_text(const catalog::ColumnType& type, const std::string& text, uint32_t display_length) {
  if (!type.zerofill || text.size() >= display_length) return text;
  return std::string(display_length - text.size(), '0') + text;
}

bool key_has_null(const catalog::Key& key, const txn::Row& row) {
  for (const auto& part : key.parts)
    if (!row[part.column]) return true;
  return false;
}

std::string encode_key(const catalog::Table& table, const catalog::Key& key, const txn::Row& row) {
  std::string out;
  for (const auto& part : key.parts) {
    const auto& column = table.columns[part.column];
    const auto& value = row[part.column];
    if (!value) {
      out.push_back('\0');
      continue;
    }
    out.push_back('\1');
    const auto& type = column.type;
    const auto v = value_of(type, *value);
    if (const auto* i = std::get_if<int64_t>(&v)) {
      append_big_endian(out, static_cast<uint64_t>(*i) ^ (uint64_t{1} << 63));
    } else if (const auto* u = std::get_if<uint64_t>(&v)) {
      append_big_endian(out, *u);
    } else if (const auto* d = std::get_if<Decimal>(&v)) {
      // At the column's scale every value has the same digits after the
      // point: the digits in a field the column's precision wide, sign
      // first, and turned around when negative.
      auto digits = d->to_string();
      if (!digits.empty() && digits[0] == '-') digits.erase(0, 1);
      digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
      digits = std::string(type.length > digits.size() ? type.length - digits.size() : 0, '0') + digits;
      const bool negative = d->negative() && !d->is_zero();
      out.push_back(negative ? '\0' : '\1');
      for (const char ch : digits) out.push_back(negative ? static_cast<char>('9' - ch + '0') : ch);
    } else if (const auto* r = std::get_if<double>(&v)) {
      const double x = *r == 0 ? 0.0 : *r;
      uint64_t bits = 0;
      std::memcpy(&bits, &x, sizeof bits);
      bits = sign_of(x) < 0 ? ~bits : bits | (uint64_t{1} << 63);
      append_big_endian(out, bits);
    } else {
      auto text = std::get<std::string>(v);
      if (catalog::is_text(type.kind)) {
        const uint16_t id = column_collation_id(column);
        if (part.prefix) text.resize(character_end(text, part.prefix));
        const auto* collation = collation_by_id(id);
        if (!collation) collation = collation_by_id(46);
        if (pads(id)) {
          // Padded to the key's length in characters, as MySQL's key images
          // are, so that trailing spaces do not count.
          const uint64_t width = part.prefix ? part.prefix : catalog::is_blob(type.kind) ? characters(text) : type.length;
          const size_t have = characters(text);
          if (have < width) text.append(width - have, ' ');
          out += collation->sort_key(text);
        } else {
          append_terminated(out, collation->sort_key(text));
        }
      } else {
        if (part.prefix && text.size() > part.prefix) text.resize(part.prefix);
        append_terminated(out, text);
      }
    }
  }
  return out;
}

std::string key_text(const catalog::Table& table, const catalog::Key& key, const txn::Row& row) {
  std::string out;
  for (size_t i = 0; i < key.parts.size(); ++i) {
    if (i) out += "-";
    const auto& part = key.parts[i];
    const auto& value = row[part.column];
    if (!value) {
      out += "NULL";
      continue;
    }
    // A prefix part is named by its prefix (observed: 'ab' of 'abd' for
    // a(2)), in characters for text and bytes otherwise.
    const bool text = catalog::is_text(table.columns[part.column].type.kind);
    out += !part.prefix ? *value : value->substr(0, text ? character_end(*value, part.prefix) : std::min<size_t>(part.prefix, value->size()));
  }
  return out;
}

int compare_for_order(const Value& a, const Value& b, uint16_t collation) {
  const bool an = is_null(a), bn = is_null(b);
  if (an || bn) return an == bn ? 0 : an ? -1 : 1;
  const auto* sa = std::get_if<std::string>(&a);
  const auto* sb = std::get_if<std::string>(&b);
  if (sa && sb) {
    const auto* c = collation ? collation_by_id(collation) : nullptr;
    if (!c) c = collation_by_id(63);
    return c->compare(*sa, *sb);
  }
  const auto exact = [](const Value& v) -> std::optional<Decimal> {
    if (const auto* i = std::get_if<int64_t>(&v)) return Decimal::from_int(*i);
    if (const auto* u = std::get_if<uint64_t>(&v)) return Decimal::from_uint(*u);
    if (const auto* d = std::get_if<Decimal>(&v)) return *d;
    return std::nullopt;
  };
  const auto da = exact(a), db = exact(b);
  if (da && db) return compare(*da, *db);
  const auto as_double = [](const Value& v) {
    if (const auto* r = std::get_if<double>(&v)) return *r;
    if (const auto* i = std::get_if<int64_t>(&v)) return static_cast<double>(*i);
    if (const auto* u = std::get_if<uint64_t>(&v)) return static_cast<double>(*u);
    if (const auto* d = std::get_if<Decimal>(&v)) return d->to_double();
    return std::strtod(std::get<std::string>(v).c_str(), nullptr);
  };
  const double x = as_double(a), y = as_double(b);
  return x < y ? -1 : x > y ? 1 : 0;
}

}  // namespace kvasir::exec
