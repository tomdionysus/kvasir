//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A table's shape as the catalog holds it once CREATE TABLE has checked it:
// its columns with MySQL's data types, and its keys. What a type takes to
// store and to index is computed here, as InnoDB counts it (observed on MySQL
// 8.4.13), since both CREATE TABLE's limits and the rows to come rest on it.
namespace kvasir::catalog {

enum class TypeKind : uint8_t {
  tinyint,
  smallint,
  mediumint,
  int_,
  bigint,
  decimal,
  float_,
  double_,
  char_,
  varchar,
  binary,
  varbinary,
  tinytext,
  text,
  mediumtext,
  longtext,
  tinyblob,
  blob,
  mediumblob,
  longblob,
  date,
  time,
  datetime,
  timestamp,
  year,
};

struct ColumnType {
  TypeKind kind = TypeKind::int_;
  // CHAR and VARCHAR: characters. BINARY and VARBINARY: bytes. An integer:
  // its display width, 0 for none. DECIMAL: its precision. FLOAT and DOUBLE:
  // M of (M,D), 0 for none.
  uint32_t length = 0;
  // DECIMAL's and FLOAT(M,D)'s D; TIME's, DATETIME's and TIMESTAMP's
  // fractional seconds' digits.
  uint8_t scale = 0;
  bool is_unsigned = false;
  bool zerofill = false;
  // A text type's character set and collation; empty for the rest.
  std::string charset;
  std::string collation;
};

bool is_integer(TypeKind kind) noexcept;
bool is_numeric(TypeKind kind) noexcept;
// CHAR, VARCHAR and the TEXT types.
bool is_text(TypeKind kind) noexcept;
// BINARY, VARBINARY and the BLOB types.
bool is_binary(TypeKind kind) noexcept;
// The TEXT and BLOB types.
bool is_blob(TypeKind kind) noexcept;
// DATE, TIME, DATETIME and TIMESTAMP (YEAR is a number).
bool is_temporal(TypeKind kind) noexcept;
// The most a TEXT or BLOB type holds, in bytes.
uint64_t blob_max_bytes(TypeKind kind) noexcept;

// The widest character of a character set Kvasir holds, in bytes.
uint32_t max_bytes_per_character(const std::string& charset) noexcept;

// The bytes a value of the type takes in a row, as InnoDB counts them toward
// its 65,535-byte row limit (a TEXT or BLOB counts only its pointer).
uint32_t pack_length(const ColumnType& type) noexcept;
// The bytes the type takes in a key, `prefix` characters (or bytes) of it
// when nonzero.
uint32_t key_length(const ColumnType& type, uint32_t prefix) noexcept;

struct Column {
  std::string name;
  ColumnType type;
  bool nullable = true;
  bool auto_increment = false;
  // DEFAULT: none written, NULL, a value, held as the text a client is sent
  // for it, or CURRENT_TIMESTAMP (a DATETIME's or TIMESTAMP's, at its own
  // fraction digits).
  enum class Default : uint8_t { none, null, value, current_timestamp };
  Default default_kind = Default::none;
  std::string default_value;
  // ON UPDATE CURRENT_TIMESTAMP: an UPDATE that changes the row and does not
  // set the column sets it to the statement's time.
  bool on_update = false;
  std::string comment;
};

struct KeyPart {
  size_t column = 0;
  // Characters (bytes for a binary type) indexed; 0 for the whole value.
  uint32_t prefix = 0;
  bool descending = false;
};

struct Key {
  enum class Kind : uint8_t { primary, unique, index };
  Kind kind = Kind::index;
  std::string name;
  std::vector<KeyPart> parts;
  std::string comment;
};

struct Table {
  std::string name;
  std::vector<Column> columns;
  // In the order written.
  std::vector<Key> keys;
  std::string engine = "InnoDB";
  // The defaults its text columns took.
  std::string charset;
  std::string collation;
  uint64_t auto_increment = 1;
  std::string comment;
  // The catalog's number for it, unique on the node for its life, so a table
  // dropped and made again is another table.
  uint64_t id = 0;

  // The index of the column named, compared as MySQL compares column names
  // (case-insensitively), or nothing.
  std::optional<size_t> column(const std::string& wanted) const;
  // The key that serves as the primary key: PRIMARY KEY, else the first
  // UNIQUE key whose columns are all NOT NULL and whole (MySQL's promotion,
  // observed in the PRI_KEY flag). Nothing when there is none.
  std::optional<size_t> primary_key() const;
};

}  // namespace kvasir::catalog
