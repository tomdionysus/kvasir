//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/ddl.h"

#include <algorithm>
#include <cctype>
#include <optional>

#include "kvasir/exec/store.h"
#include "kvasir/exec/variables.h"
#include "kvasir/protocol/types.h"
#include "kvasir/sql/keywords.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;
using catalog::TypeKind;

// MySQL's limits.
constexpr size_t most_name_characters = 64;
constexpr size_t most_columns = 4096;
constexpr size_t most_keys = 64;
constexpr size_t most_key_parts = 16;
constexpr uint32_t most_key_bytes = 3072;
constexpr uint32_t most_row_bytes = 65535;

[[noreturn]] void refuse(uint16_t code, const char* state, const std::string& message) { throw SqlError(code, state, message); }

[[noreturn]] void not_yet(const std::string& what) {
  throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support '" + what + "'");
}

std::string lower(std::string s) {
  for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

size_t characters(const std::string& s) {
  size_t n = 0;
  for (const char ch : s)
    if ((static_cast<uint8_t>(ch) & 0xc0) != 0x80) ++n;
  return n;
}

const Warning width_warning{"Warning", 1681, "Integer display width is deprecated and will be removed in a future release."};
const Warning zerofill_warning{"Warning", 1681,
                               "The ZEROFILL attribute is deprecated and will be removed in a future release. Use the LPAD function to zero-pad numbers, "
                               "or store the formatted numbers in a CHAR column."};
const Warning digits_warning{"Warning", 1681,
                             "Specifying number of digits for floating point data types is deprecated and will be removed in a future release."};
const Warning unsigned_warning{"Warning", 1681,
                               "UNSIGNED for decimal and floating point data types is deprecated and support for it will be removed in a future release."};

bool integer_name(const std::string& name) { return name == "TINYINT" || name == "SMALLINT" || name == "MEDIUMINT" || name == "INT" || name == "BIGINT"; }

// The collations a column can take: a session's, and utf8mb3's two, which a
// column holds though no session yet speaks utf8mb3.
const CollationInfo utf8mb3_general_ci{"utf8mb3_general_ci", 33, "utf8mb3", true};
const CollationInfo utf8mb3_bin{"utf8mb3_bin", 83, "utf8mb3", false};

const CollationInfo* collation_named(std::string_view name) {
  if (const auto* found = find_collation(name)) return found;
  for (const auto* c : {&utf8mb3_general_ci, &utf8mb3_bin})
    if (sql::same_word(c->name, name)) return c;
  return nullptr;
}

const CollationInfo* charset_default(std::string_view charset) {
  if (sql::same_word(charset, "utf8mb3")) return &utf8mb3_general_ci;
  return default_collation(charset);
}

// A character set as CREATE TABLE names it: its default collation.
const CollationInfo& named_charset(const std::string& name) {
  if (const auto* found = charset_default(name)) return *found;
  if (charset_not_converted(name)) not_yet("character set " + lower(name));
  refuse(1115, "42000", "Unknown character set: '" + name + "'");
}

// CHARACTER SET, the BINARY attribute and COLLATE together, over `fallback`.
const CollationInfo& resolve(const std::string& charset, bool binary_attribute, const std::string& collation, const CollationInfo& fallback) {
  const CollationInfo* chosen = charset.empty() ? nullptr : &named_charset(charset);
  if (binary_attribute) {
    const auto& base = chosen ? *chosen : fallback;
    if (base.charset != "binary") chosen = collation_named(base.charset + "_bin");
  }
  if (!collation.empty()) {
    const auto* named = collation_named(collation);
    const std::string against = chosen ? chosen->charset : fallback.charset;
    if (!named) {
      if (charset_not_converted(std::string_view(collation).substr(0, collation.find('_'))))
        refuse(1253, "42000", "COLLATION '" + collation + "' is not valid for CHARACTER SET '" + against + "'");
      refuse(1273, "HY000", "Unknown collation: '" + collation + "'");
    }
    if (chosen && named->charset != chosen->charset)
      refuse(1253, "42000", "COLLATION '" + collation + "' is not valid for CHARACTER SET '" + chosen->charset + "'");
    chosen = named;
  }
  return chosen ? *chosen : fallback;
}

bool is_bin(const std::string& collation) { return collation.size() > 4 && collation.compare(collation.size() - 4, 4, "_bin") == 0; }

[[noreturn]] void too_long(const std::string& column, uint64_t most) {
  refuse(1074, "42000", "Column length too big for column '" + column + "' (max = " + std::to_string(most) + "); use BLOB or TEXT instead");
}

// M and D of DECIMAL(M,D), FLOAT(M,D) and DOUBLE(M,D), checked in MySQL's
// order.
void check_scale(const std::string& column, uint64_t m, uint64_t d) {
  if (d > 30) refuse(1425, "42000", "Too big scale " + std::to_string(d) + " specified for column '" + column + "'. Maximum is 30.");
  if (m < d) refuse(1427, "42000", "For float(M,D), double(M,D) or decimal(M,D), M must be >= D (column '" + column + "').");
}

void check_width(const std::string& column, uint64_t width) {
  if (width > 255) refuse(1439, "42000", "Display width out of range for column '" + column + "' (max = 255)");
}

TypeKind blob_kind(uint64_t bytes, bool binary) {
  if (bytes <= 255) return binary ? TypeKind::tinyblob : TypeKind::tinytext;
  if (bytes <= 65535) return binary ? TypeKind::blob : TypeKind::text;
  if (bytes <= 16777215) return binary ? TypeKind::mediumblob : TypeKind::mediumtext;
  return binary ? TypeKind::longblob : TypeKind::longtext;
}

TypeKind as_binary(TypeKind kind) {
  switch (kind) {
    case TypeKind::char_:
      return TypeKind::binary;
    case TypeKind::varchar:
      return TypeKind::varbinary;
    case TypeKind::tinytext:
      return TypeKind::tinyblob;
    case TypeKind::text:
      return TypeKind::blob;
    case TypeKind::mediumtext:
      return TypeKind::mediumblob;
    case TypeKind::longtext:
      return TypeKind::longblob;
    default:
      return kind;
  }
}

// A column's type from its written form.
catalog::ColumnType column_type(const sql::ColumnSpec& column, const CollationInfo& table_collation) {
  const auto& d = column.type;
  const auto& name = column.name;
  catalog::ColumnType t;
  t.is_unsigned = d.is_unsigned || d.zerofill;
  t.zerofill = d.zerofill;
  if (integer_name(d.name) || d.name == "BOOL") {
    t.kind = d.name == "TINYINT" || d.name == "BOOL" ? TypeKind::tinyint
             : d.name == "SMALLINT"                  ? TypeKind::smallint
             : d.name == "MEDIUMINT"                 ? TypeKind::mediumint
             : d.name == "INT"                       ? TypeKind::int_
                                                     : TypeKind::bigint;
    if (d.length) check_width(name, *d.length);
    t.length = d.name == "BOOL" ? 1 : static_cast<uint32_t>(d.length.value_or(0));
    return t;
  }
  if (d.name == "DECIMAL") {
    uint64_t m = d.length.value_or(10);
    if (m == 0) m = 10;
    if (m > 65) refuse(1426, "42000", "Too-big precision " + std::to_string(m) + " specified for '" + name + "'. Maximum is 65.");
    const uint64_t s = d.scale.value_or(0);
    check_scale(name, m, s);
    t.kind = TypeKind::decimal;
    t.length = static_cast<uint32_t>(m);
    t.scale = static_cast<uint8_t>(s);
    return t;
  }
  if (d.name == "FLOAT" || d.name == "DOUBLE") {
    t.kind = d.name == "FLOAT" ? TypeKind::float_ : TypeKind::double_;
    if (d.length && !d.scale) {
      // FLOAT(p): a precision in bits, FLOAT to 24, DOUBLE to 53.
      if (*d.length > 53) refuse(1063, "42000", "Incorrect column specifier for column '" + name + "'");
      t.kind = *d.length > 24 ? TypeKind::double_ : TypeKind::float_;
    } else if (d.length) {
      check_width(name, *d.length);
      check_scale(name, *d.length, *d.scale);
      t.length = static_cast<uint32_t>(*d.length);
      t.scale = static_cast<uint8_t>(*d.scale);
    }
    return t;
  }
  // The temporal types: YEAR as MySQL 8.4 keeps it (4 digits, UNSIGNED
  // ZEROFILL), the rest with fractional seconds up to 6 digits (1426).
  if (d.name == "YEAR") {
    if (d.length && *d.length != 4) refuse(1818, "HY000", "Invalid display width. Use YEAR instead.");
    t.kind = TypeKind::year;
    t.length = 4;
    t.is_unsigned = true;
    t.zerofill = true;
    return t;
  }
  if (d.name == "DATE" || d.name == "TIME" || d.name == "DATETIME" || d.name == "TIMESTAMP") {
    const uint64_t fsp = d.length.value_or(0);
    if (fsp > 6) refuse(1426, "42000", "Too-big precision " + std::to_string(fsp) + " specified for '" + name + "'. Maximum is 6.");
    t.is_unsigned = false;
    t.zerofill = false;
    t.kind = d.name == "DATE" ? TypeKind::date : d.name == "TIME" ? TypeKind::time : d.name == "DATETIME" ? TypeKind::datetime : TypeKind::timestamp;
    t.scale = static_cast<uint8_t>(fsp);
    return t;
  }
  // The string types.
  t.is_unsigned = false;
  t.zerofill = false;
  TypeKind kind = TypeKind::char_;
  uint64_t length = d.length.value_or(1);
  if (d.name == "CHAR") {
    kind = TypeKind::char_;
  } else if (d.name == "VARCHAR") {
    kind = TypeKind::varchar;
  } else if (d.name == "BINARY" || d.name == "VARBINARY") {
    kind = d.name == "BINARY" ? TypeKind::binary : TypeKind::varbinary;
  } else if (d.name == "TEXT" || d.name == "BLOB") {
    // Sized below, once the character set is known.
    kind = TypeKind::text;
  } else if (d.name == "TINYTEXT") {
    kind = TypeKind::tinytext;
  } else if (d.name == "MEDIUMTEXT") {
    kind = TypeKind::mediumtext;
  } else if (d.name == "LONGTEXT") {
    kind = TypeKind::longtext;
  } else if (d.name == "TINYBLOB") {
    kind = TypeKind::tinyblob;
  } else if (d.name == "MEDIUMBLOB") {
    kind = TypeKind::mediumblob;
  } else {
    // LONGBLOB: the parser gives no other name (one not built is refused
    // there).
    kind = TypeKind::longblob;
  }
  const bool binary_type = catalog::is_binary(kind) || d.name == "BLOB";
  std::string charset = "binary";
  std::string collation = "binary";
  if (!binary_type) {
    const auto& chosen = resolve(d.charset, d.binary_attribute, d.collation, table_collation);
    charset = chosen.charset;
    collation = chosen.name;
  }
  const uint32_t bytes_per_character = catalog::max_bytes_per_character(charset);
  if (d.name == "TEXT" || d.name == "BLOB") {
    // TEXT(M) and BLOB(M): the smallest type that holds M characters.
    kind = blob_kind(d.length ? *d.length * (d.name == "TEXT" ? bytes_per_character : 1) : 65535, d.name == "BLOB");
  }
  if (charset == "binary") kind = as_binary(kind);
  switch (kind) {
    case TypeKind::char_:
    case TypeKind::binary:
      if (length > 255) too_long(name, 255);
      break;
    case TypeKind::varchar:
      if (length * bytes_per_character > 65535) too_long(name, 65535 / bytes_per_character);
      break;
    case TypeKind::varbinary:
      if (length > 65535) too_long(name, 65535);
      break;
    default:
      length = 0;
      break;
  }
  t.kind = kind;
  t.length = static_cast<uint32_t>(length);
  if (catalog::is_text(kind)) {
    t.charset = charset;
    t.collation = collation;
  }
  return t;
}

void check_name(const std::string& name, uint16_t code, const char* what) {
  if (characters(name) > most_name_characters) refuse(1059, "42000", "Identifier name '" + name + "' is too long");
  if (name.empty() || name.back() == ' ') refuse(code, "42000", std::string("Incorrect ") + what + " name '" + name + "'");
}

[[noreturn]] void invalid_default(const std::string& column) { refuse(1067, "42000", "Invalid default value for '" + column + "'"); }

// Whether a DEFAULT's or ON UPDATE's call is CURRENT_TIMESTAMP (or NOW,
// LOCALTIME, LOCALTIMESTAMP) as a DATETIME or TIMESTAMP column of `type`
// takes it: with the column's own fraction digits.
bool current_timestamp(const sql::Expr& e, const catalog::ColumnType& type) {
  if (e.kind != sql::ExprKind::call) return false;
  if (e.value != "CURRENT_TIMESTAMP" && e.value != "NOW" && e.value != "LOCALTIME" && e.value != "LOCALTIMESTAMP") return false;
  if (type.kind != TypeKind::datetime && type.kind != TypeKind::timestamp) return false;
  uint64_t fsp = 0;
  if (!e.args.empty()) {
    if (e.args[0]->kind != sql::ExprKind::integer || e.args[0]->value.size() > 2) return false;
    fsp = std::stoull(e.args[0]->value);
  }
  return fsp == type.scale;
}

// A column's DEFAULT, stored as the column would store it.
void set_default(catalog::Column& column, const sql::Expr& e, const TableContext& context, std::vector<Warning>& warnings) {
  if (catalog::is_blob(column.type.kind) && e.kind != sql::ExprKind::null)
    refuse(1101, "42000", "BLOB, TEXT, GEOMETRY or JSON column '" + column.name + "' can't have a default value");
  // DEFAULT CURRENT_TIMESTAMP (or NOW, LOCALTIME, LOCALTIMESTAMP): a
  // DATETIME's or TIMESTAMP's, at its own fraction digits (1067 else).
  if (e.kind == sql::ExprKind::call && !column.auto_increment && current_timestamp(e, column.type)) {
    column.default_kind = catalog::Column::Default::current_timestamp;
    return;
  }
  if (column.auto_increment || e.kind == sql::ExprKind::call) invalid_default(column.name);
  if (e.kind == sql::ExprKind::null) {
    if (!column.nullable) invalid_default(column.name);
    column.default_kind = catalog::Column::Default::null;
    return;
  }
  Evaluator evaluator(context.eval);
  const auto from = evaluator.type(e);
  const auto value = evaluator.value(e);
  const auto stored = store(column.type, value, from);
  switch (stored.result) {
    case Stored::Result::truncated:
      warnings.push_back({"Note", 1265, "Data truncated for column '" + column.name + "' at row 1"});
      [[fallthrough]];
    case Stored::Result::stored:
      column.default_kind = catalog::Column::Default::value;
      column.default_value = stored.text;
      return;
    default:
      invalid_default(column.name);
  }
}

using KeyKind = catalog::Key::Kind;

// The keys in the order MySQL takes them: each column's own after it, table
// keys where they stand among the columns.
struct PendingKey {
  const sql::KeySpec* spec = nullptr;
  // A column's own: its index and kind.
  size_t column = 0;
  KeyKind kind = KeyKind::index;
};

std::vector<PendingKey> keys_in_order(const sql::CreateTable& spec) {
  std::vector<PendingKey> out;
  for (size_t i = 0; i <= spec.columns.size(); ++i) {
    for (const auto& k : spec.keys)
      if (k.position == i) out.push_back({&k, 0, KeyKind::index});
    if (i == spec.columns.size()) break;
    if (spec.columns[i].primary) out.push_back({nullptr, i, KeyKind::primary});
    if (spec.columns[i].unique) out.push_back({nullptr, i, KeyKind::unique});
  }
  return out;
}

bool same_parts(const catalog::Key& a, const catalog::Key& b) {
  if (a.parts.size() != b.parts.size()) return false;
  for (size_t i = 0; i < a.parts.size(); ++i)
    if (a.parts[i].column != b.parts[i].column || a.parts[i].prefix != b.parts[i].prefix || a.parts[i].descending != b.parts[i].descending) return false;
  return true;
}

void add_keys(const sql::CreateTable& spec, catalog::Table& table, const TableContext& context, std::vector<Warning>& warnings) {
  const auto taken = [&](const std::string& name) {
    return std::any_of(table.keys.begin(), table.keys.end(), [&](const catalog::Key& k) { return sql::same_word(k.name, name); });
  };
  for (const auto& pending : keys_in_order(spec)) {
    catalog::Key key;
    std::string name;
    if (pending.spec) {
      const auto& s = *pending.spec;
      key.kind = s.kind == sql::KeySpec::Kind::primary ? KeyKind::primary : s.kind == sql::KeySpec::Kind::unique ? KeyKind::unique : KeyKind::index;
      key.comment = s.comment;
      name = s.name;
      if (s.parts.size() > most_key_parts) refuse(1070, "42000", "Too many key parts specified; max 16 parts allowed");
      for (const auto& part : s.parts) {
        const auto column = table.column(part.column);
        if (!column) refuse(1072, "42000", "Key column '" + part.column + "' doesn't exist in table");
        key.parts.push_back({*column, static_cast<uint32_t>(std::min<uint64_t>(part.prefix.value_or(0), UINT32_MAX)), part.descending});
      }
    } else {
      key.kind = pending.kind;
      key.parts.push_back({pending.column, 0, false});
    }
    if (key.kind == KeyKind::primary) {
      if (std::any_of(table.keys.begin(), table.keys.end(), [](const catalog::Key& k) { return k.kind == KeyKind::primary; }))
        refuse(1068, "42000", "Multiple primary key defined");
      name = "PRIMARY";
    } else if (!name.empty()) {
      if (sql::same_word(name, "PRIMARY")) refuse(1280, "42000", "Incorrect index name '" + name + "'");
      if (taken(name)) refuse(1061, "42000", "Duplicate key name '" + name + "'");
    } else {
      const auto& base = table.columns[key.parts[0].column].name;
      name = base;
      for (int n = 2; taken(name) || sql::same_word(name, "PRIMARY"); ++n) name = base + "_" + std::to_string(n);
    }
    key.name = name;
    uint32_t bytes = 0;
    for (auto& part : key.parts) {
      auto& column = table.columns[part.column];
      const auto& type = column.type;
      const bool string_type = catalog::is_text(type.kind) || catalog::is_binary(type.kind);
      if (part.prefix) {
        const uint64_t whole = catalog::is_blob(type.kind) ? catalog::blob_max_bytes(type.kind) : type.length;
        const std::string message =
            "Incorrect prefix key; the used key part isn't a string, the used length is longer than the key part, or the storage engine doesn't support "
            "unique prefix keys";
        if (!string_type || part.prefix > whole) refuse(1089, "HY000", message);
        // A prefix the whole column long is the whole column.
        if (!catalog::is_blob(type.kind) && part.prefix == type.length) part.prefix = 0;
      } else if (catalog::is_blob(type.kind)) {
        refuse(1170, "42000", "BLOB/TEXT column '" + column.name + "' used in key specification without a key length");
      }
      if (key.kind == KeyKind::primary) {
        if (spec.columns[part.column].nullable.value_or(false))
          refuse(1171, "42000", "All parts of a PRIMARY KEY must be NOT NULL; if you need NULL in a key, use UNIQUE instead");
        column.nullable = false;
      }
      bytes += catalog::key_length(type, part.prefix);
    }
    if (bytes > most_key_bytes) refuse(1071, "42000", "Specified key was too long; max key length is 3072 bytes");
    // The same columns indexed again the same way: allowed, with a warning.
    for (const auto& earlier : table.keys) {
      if ((earlier.kind == KeyKind::index) == (key.kind == KeyKind::index) && earlier.kind != KeyKind::primary && key.kind != KeyKind::primary &&
          same_parts(earlier, key)) {
        warnings.push_back({"Warning", 1831,
                            "Duplicate index '" + key.name + "' defined on the table '" + context.database + "." + table.name +
                                "'. This is deprecated and will be disallowed in a future release."});
        break;
      }
    }
    table.keys.push_back(std::move(key));
    if (table.keys.size() > most_keys) refuse(1069, "42000", "Too many keys specified; max 64 keys allowed");
  }
}

}  // namespace

void type_warnings(const sql::CreateTable& spec, std::vector<Warning>& warnings) {
  for (const auto& column : spec.columns) {
    const auto& d = column.type;
    if (integer_name(d.name)) {
      if (d.length) warnings.push_back(width_warning);
      if (d.zerofill) warnings.push_back(zerofill_warning);
    } else if (d.name == "DECIMAL" || d.name == "FLOAT" || d.name == "DOUBLE") {
      if (d.name != "DECIMAL" && d.scale) warnings.push_back(digits_warning);
      if (d.zerofill) warnings.push_back(zerofill_warning);
      if (d.is_unsigned) warnings.push_back(unsigned_warning);
    }
  }
}

catalog::Table define_table(const sql::CreateTable& spec, const TableContext& context, std::vector<Warning>& warnings) {
  catalog::Table table;
  table.name = spec.name.back();
  table.comment = spec.comment;
  table.auto_increment = spec.auto_increment.value_or(1);

  // The table's character set and collation, over the database's.
  const auto* database_collation = collation_named(context.collation);
  if (!database_collation) database_collation = charset_default(context.charset);
  const auto& table_collation = resolve(spec.charset, false, spec.collation, *database_collation);
  table.charset = table_collation.charset;
  table.collation = table_collation.name;

  if (!spec.engine.empty() && !sql::same_word(spec.engine, "InnoDB")) {
    const std::string unknown = "Unknown storage engine '" + spec.engine + "'";
    if (context.no_engine_substitution) refuse(1286, "42000", unknown);
    warnings.push_back({"Warning", 1286, unknown});
    warnings.push_back({"Warning", 1266, "Using storage engine InnoDB for table '" + table.name + "'"});
  }

  if (spec.columns.size() > most_columns) refuse(1117, "HY000", "Too many columns");
  for (const auto& written : spec.columns) {
    check_name(written.name, 1166, "column");
    if (table.column(written.name)) refuse(1060, "42S21", "Duplicate column name '" + written.name + "'");
    catalog::Column column;
    column.name = written.name;
    column.comment = written.comment;
    column.type = column_type(written, table_collation);
    column.auto_increment = written.auto_increment;
    if (written.auto_increment && !catalog::is_integer(column.type.kind)) refuse(1063, "42000", "Incorrect column specifier for column '" + written.name + "'");
    // PRIMARY KEY and AUTO_INCREMENT make a column NOT NULL.
    column.nullable = written.nullable.value_or(true) && !written.primary && !written.auto_increment;
    if (written.default_value) set_default(column, *written.default_value, context, warnings);
    // ON UPDATE CURRENT_TIMESTAMP: a DATETIME's or TIMESTAMP's, at its own
    // fraction digits (1294 else, observed).
    if (written.on_update) {
      if (!current_timestamp(*written.on_update, column.type)) refuse(1294, "HY000", "Invalid ON UPDATE clause for '" + column.name + "' column");
      column.on_update = true;
    }
    table.columns.push_back(std::move(column));
  }

  add_keys(spec, table, context, warnings);

  // One AUTO_INCREMENT column, first in some key.
  size_t autos = 0;
  for (size_t i = 0; i < table.columns.size(); ++i) {
    if (!table.columns[i].auto_increment) continue;
    ++autos;
    const bool leads = std::any_of(table.keys.begin(), table.keys.end(), [&](const catalog::Key& k) { return k.parts.front().column == i; });
    if (autos > 1 || !leads) refuse(1075, "42000", "Incorrect table definition; there can be only one auto column and it must be defined as a key");
  }

  // The row limit, as InnoDB counts it: each column's bytes, and a byte for
  // each eight nullable columns.
  uint64_t row = 0;
  size_t nullable = 0;
  for (const auto& column : table.columns) {
    row += catalog::pack_length(column.type);
    if (column.nullable) ++nullable;
  }
  row += (nullable + 7) / 8;
  if (row > most_row_bytes)
    refuse(1118, "42000",
           "Row size too large. The maximum row size for the used table type, not counting BLOBs, is 65535. This includes storage overhead, check the "
           "manual. You have to change some columns to TEXT or BLOBs");
  return table;
}

void check_table_name(const std::string& name) { check_name(name, 1103, "table"); }

uint16_t column_collation_id(const catalog::Column& column) {
  if (catalog::is_text(column.type.kind)) {
    const auto* info = collation_named(column.type.collation);
    return info ? info->id : 0;
  }
  return catalog::is_binary(column.type.kind) ? protocol::collation::binary : 0;
}

ResultType column_result_type(const catalog::Table& table, size_t index, const EvalSettings&) {
  const auto& column = table.columns[index];
  const auto& t = column.type;
  const uint16_t own = column_collation_id(column);
  ResultType r;
  r.collation = protocol::collation::binary;
  uint32_t flags = 0;
  const auto primary = table.primary_key();
  for (size_t k = 0; k < table.keys.size(); ++k) {
    const auto& key = table.keys[k];
    for (size_t p = 0; p < key.parts.size(); ++p) {
      if (key.parts[p].column != index) continue;
      flags |= c::part_key;
      if (primary == k) {
        flags |= c::primary_key;
      } else if (p == 0) {
        flags |= key.kind != KeyKind::index && key.parts.size() == 1 ? c::unique_key : c::multiple_key;
      }
    }
  }
  if (!column.nullable) flags |= c::not_null;
  if (column.auto_increment) flags |= c::auto_increment;
  if (!column.nullable && column.default_kind == catalog::Column::Default::none && !column.auto_increment) flags |= c::no_default_value;
  if (t.is_unsigned) flags |= c::unsigned_;
  if (t.zerofill) flags |= c::zerofill;
  const auto integer = [&](uint8_t type, uint32_t signed_length, uint32_t unsigned_length) {
    r.field_type = type;
    r.length = t.length ? t.length : t.is_unsigned ? unsigned_length : signed_length;
    // Arithmetic counts the type's own digits, whatever the display width
    // (observed: INT(5) ZEROFILL + 0 is 11 long).
    r.precision = static_cast<int>(t.is_unsigned ? unsigned_length : signed_length - 1);
    flags |= c::num;
  };
  // A column's own type: its collation, its length in its own character
  // set's bytes; a TEXT's is its bytes.
  const auto text_length = [&](uint64_t count) { return static_cast<uint32_t>(std::min<uint64_t>(count * widest_character(own), UINT32_MAX)); };
  switch (t.kind) {
    case TypeKind::tinyint:
      integer(f::tiny, 4, 3);
      break;
    case TypeKind::smallint:
      integer(f::short_, 6, 5);
      break;
    case TypeKind::mediumint:
      integer(f::int24, 9, 8);
      break;
    case TypeKind::int_:
      integer(f::long_, 11, 10);
      break;
    case TypeKind::bigint:
      integer(f::longlong, 20, 20);
      break;
    case TypeKind::decimal:
      r.field_type = f::newdecimal;
      r.length = t.length + (t.scale > 0 ? 1 : 0) + (t.is_unsigned ? 0 : 1);
      r.decimals = t.scale;
      r.precision = static_cast<int>(t.length);
      flags |= c::num;
      break;
    case TypeKind::float_:
    case TypeKind::double_:
      r.field_type = t.kind == TypeKind::float_ ? f::float_ : f::double_;
      r.length = t.length ? t.length : t.kind == TypeKind::float_ ? 12u : 22u;
      r.decimals = static_cast<uint8_t>(t.length ? t.scale : protocol::not_fixed_decimals);
      flags |= c::num;
      break;
    case TypeKind::char_:
    case TypeKind::varchar:
      r.field_type = t.kind == TypeKind::char_ ? f::string : f::var_string;
      r.collation = own;
      r.length = text_length(t.length);
      if (is_bin(t.collation)) flags |= c::binary;
      break;
    case TypeKind::binary:
    case TypeKind::varbinary:
      r.field_type = t.kind == TypeKind::binary ? f::string : f::var_string;
      r.length = t.length;
      flags |= c::binary;
      break;
    case TypeKind::tinytext:
    case TypeKind::text:
    case TypeKind::mediumtext:
    case TypeKind::longtext:
      r.field_type = f::blob;
      r.collation = own;
      r.length = static_cast<uint32_t>(catalog::blob_max_bytes(t.kind));
      flags |= c::blob;
      if (is_bin(t.collation)) flags |= c::binary;
      break;
    case TypeKind::tinyblob:
    case TypeKind::blob:
    case TypeKind::mediumblob:
    case TypeKind::longblob:
      r.field_type = f::blob;
      r.length = static_cast<uint32_t>(catalog::blob_max_bytes(t.kind));
      flags |= c::blob | c::binary;
      break;
    // DATE 10 long; TIME 10, DATETIME and TIMESTAMP 19, each with a point and
    // its fraction's digits; all BINARY (observed). The digits each carries
    // into arithmetic: 8, 7, 14.
    case TypeKind::date:
    case TypeKind::time:
    case TypeKind::datetime:
    case TypeKind::timestamp: {
      const uint32_t fraction = t.scale ? t.scale + 1u : 0u;
      r.decimals = t.scale;
      if (t.kind == TypeKind::date) {
        r.field_type = f::date;
        r.length = 10;
        r.precision = 8;
      } else if (t.kind == TypeKind::time) {
        r.field_type = f::time;
        r.length = 10 + fraction;
        r.precision = 7 + t.scale;
      } else {
        r.field_type = t.kind == TypeKind::datetime ? f::datetime : f::timestamp;
        r.length = 19 + fraction;
        r.precision = 14 + t.scale;
      }
      flags |= c::binary;
      // A TIMESTAMP set by the clock is flagged so (observed: TIMESTAMP and
      // ON_UPDATE_NOW for one with both; a DATETIME has neither).
      if (t.kind == TypeKind::timestamp && (column.on_update || column.default_kind == catalog::Column::Default::current_timestamp)) flags |= c::timestamp;
      if (t.kind == TypeKind::timestamp && column.on_update) flags |= c::on_update_now;
      break;
    }
    // YEAR: 4 long, UNSIGNED ZEROFILL NUM (observed).
    case TypeKind::year:
      r.field_type = f::year;
      r.length = 4;
      r.precision = 4;
      flags |= c::num;
      break;
  }
  r.flags = static_cast<uint16_t>(flags);
  return r;
}

}  // namespace kvasir::exec
