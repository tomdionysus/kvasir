//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/catalog/table.h"

#include <array>

namespace kvasir::catalog {

namespace {

// A DECIMAL's bytes: four for each nine digits, and MySQL's table for the rest,
// either side of the point.
uint32_t decimal_digits_bytes(uint32_t digits) {
  constexpr std::array<uint32_t, 9> rest = {0, 1, 1, 2, 2, 3, 3, 4, 4};
  return digits / 9 * 4 + rest[digits % 9];
}

bool same_name(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    auto x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
    if (x >= 'A' && x <= 'Z') x = static_cast<unsigned char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<unsigned char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

}  // namespace

bool is_integer(TypeKind kind) noexcept { return kind <= TypeKind::bigint; }

bool is_numeric(TypeKind kind) noexcept { return kind <= TypeKind::double_; }

bool is_text(TypeKind kind) noexcept {
  return kind == TypeKind::char_ || kind == TypeKind::varchar || (kind >= TypeKind::tinytext && kind <= TypeKind::longtext);
}

bool is_binary(TypeKind kind) noexcept {
  return kind == TypeKind::binary || kind == TypeKind::varbinary || (kind >= TypeKind::tinyblob && kind <= TypeKind::longblob);
}

bool is_blob(TypeKind kind) noexcept { return kind >= TypeKind::tinytext; }

uint64_t blob_max_bytes(TypeKind kind) noexcept {
  switch (kind) {
    case TypeKind::tinytext:
    case TypeKind::tinyblob:
      return 255;
    case TypeKind::text:
    case TypeKind::blob:
      return 65535;
    case TypeKind::mediumtext:
    case TypeKind::mediumblob:
      return 16777215;
    case TypeKind::longtext:
    case TypeKind::longblob:
      return 4294967295u;
    default:
      return 0;
  }
}

uint32_t max_bytes_per_character(const std::string& charset) noexcept {
  if (charset == "utf8mb4") return 4;
  if (charset == "utf8mb3") return 3;
  return 1;
}

uint32_t pack_length(const ColumnType& type) noexcept {
  switch (type.kind) {
    case TypeKind::tinyint:
      return 1;
    case TypeKind::smallint:
      return 2;
    case TypeKind::mediumint:
      return 3;
    case TypeKind::int_:
    case TypeKind::float_:
      return 4;
    case TypeKind::bigint:
    case TypeKind::double_:
      return 8;
    case TypeKind::decimal:
      return decimal_digits_bytes(type.length - type.scale) + decimal_digits_bytes(type.scale);
    case TypeKind::char_:
      return type.length * max_bytes_per_character(type.charset);
    case TypeKind::binary:
      return type.length;
    case TypeKind::varchar:
    case TypeKind::varbinary: {
      const uint32_t bytes = type.kind == TypeKind::varchar ? type.length * max_bytes_per_character(type.charset) : type.length;
      return bytes + (bytes > 255 ? 2 : 1);
    }
    case TypeKind::tinytext:
    case TypeKind::tinyblob:
      return 9;
    case TypeKind::text:
    case TypeKind::blob:
      return 10;
    case TypeKind::mediumtext:
    case TypeKind::mediumblob:
      return 11;
    case TypeKind::longtext:
    case TypeKind::longblob:
      return 12;
  }
  return 0;
}

uint32_t key_length(const ColumnType& type, uint32_t prefix) noexcept {
  if (is_text(type.kind)) return (prefix ? prefix : type.length) * max_bytes_per_character(type.charset);
  if (is_binary(type.kind)) return prefix ? prefix : type.length;
  return pack_length(type);
}

std::optional<size_t> Table::column(const std::string& wanted) const {
  for (size_t i = 0; i < columns.size(); ++i)
    if (same_name(columns[i].name, wanted)) return i;
  return std::nullopt;
}

std::optional<size_t> Table::primary_key() const {
  for (size_t i = 0; i < keys.size(); ++i)
    if (keys[i].kind == Key::Kind::primary) return i;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (keys[i].kind != Key::Kind::unique) continue;
    bool whole = true;
    for (const auto& part : keys[i].parts)
      if (part.prefix != 0 || columns.at(part.column).nullable) whole = false;
    if (whole) return i;
  }
  return std::nullopt;
}

}  // namespace kvasir::catalog
