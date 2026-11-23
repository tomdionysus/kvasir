//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/information_schema.h"

#include <algorithm>
#include <array>

#include "kvasir/protocol/types.h"
#include "kvasir/sql/keywords.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;
using catalog::TypeKind;

// How MySQL holds the views' text: names in utf8mb3_bin, the rest in
// utf8mb3_general_ci (observed: reported in the results' collation, names
// keeping BINARY, at three bytes a character).
constexpr uint16_t name_collation = 83;
constexpr uint16_t text_collation = 33;

constexpr uint16_t nn = c::not_null, uk = c::unique_key, mk = c::multiple_key, bin = c::binary, en = c::enum_, nd = c::no_default_value, pk = c::part_key,
                   num = c::num, uns = c::unsigned_, blb = c::blob;

// A view's column: its name and the type MySQL holds it in.
struct Spec {
  const char* name;
  uint8_t field_type;
  uint16_t collation;
  uint32_t length;
  uint8_t decimals;
  uint16_t flags;
};

ResultType type_of(const Spec& s) {
  ResultType t;
  t.field_type = s.field_type;
  t.collation = s.collation;
  t.length = s.length;
  t.decimals = s.decimals;
  t.flags = s.flags;
  // An integer's digits: its length less a sign.
  if (s.field_type == f::long_ || s.field_type == f::longlong) t.precision = static_cast<decltype(t.precision)>(s.length - ((s.flags & uns) ? 0 : 1));
  return t;
}

// A column whose type differs when MySQL reads the view through a
// temporary table (ORDER BY, GROUP BY, DISTINCT): `ordered`.
void add(View& v, const Spec& s, const Spec& ordered, std::string refused = {}) {
  catalog::Column column;
  column.name = s.name;
  if (s.collation == protocol::collation::binary) {
    // A number, read as one; LONGLONG and LONG alike, unsigned as flagged.
    column.type.kind = s.field_type == f::long_ ? TypeKind::int_ : TypeKind::bigint;
    column.type.is_unsigned = (s.flags & uns) != 0;
  } else {
    column.type.kind = TypeKind::varchar;
    column.type.charset = "utf8mb3";
    column.type.collation = s.collation == name_collation ? "utf8mb3_bin" : "utf8mb3_general_ci";
    column.type.length = s.length / 3;
  }
  column.nullable = (s.flags & nn) == 0;
  v.table.columns.push_back(std::move(column));
  v.types.push_back(type_of(s));
  v.ordered.push_back(type_of(ordered));
  v.refused.push_back(std::move(refused));
}

void add(View& v, const Spec& s, std::string refused = {}) { add(v, s, s, std::move(refused)); }

std::optional<std::string> text(const std::string& s) { return s; }

const std::string& kind_name(TypeKind k) {
  static const std::array<std::string, 25> names = {"tinyint",    "smallint", "mediumint", "int",      "bigint",   "decimal",    "float",    "double",   "char",
                                                    "varchar",    "binary",   "varbinary", "tinytext", "text",     "mediumtext", "longtext", "tinyblob", "blob",
                                                    "mediumblob", "longblob", "date",      "time",     "datetime", "timestamp",  "year"};
  return names[static_cast<size_t>(k)];
}

// The column's type as MySQL writes it (observed): a display width only for
// TINYINT(1) and a ZEROFILL column, which is unsigned.
std::string column_type(const catalog::ColumnType& t) {
  std::string out = kind_name(t.kind);
  const bool is_unsigned = t.is_unsigned || t.zerofill;
  if (catalog::is_integer(t.kind)) {
    uint32_t width = t.length;
    if (t.zerofill && width == 0) {
      switch (t.kind) {
        case TypeKind::tinyint:
          width = 3;
          break;
        case TypeKind::smallint:
          width = 5;
          break;
        case TypeKind::mediumint:
          width = 8;
          break;
        case TypeKind::int_:
          width = 10;
          break;
        default:
          width = 20;
      }
    }
    if (t.zerofill || (t.kind == TypeKind::tinyint && t.length == 1)) out += "(" + std::to_string(width) + ")";
  } else if (t.kind == TypeKind::decimal) {
    out += "(" + std::to_string(t.length) + "," + std::to_string(t.scale) + ")";
  } else if (t.kind == TypeKind::float_ || t.kind == TypeKind::double_) {
    if (t.length) out += "(" + std::to_string(t.length) + "," + std::to_string(t.scale) + ")";
  } else if (catalog::is_temporal(t.kind) || t.kind == TypeKind::year) {
    // time(3), datetime(6); YEAR as year (observed).
    if (t.scale) out += "(" + std::to_string(t.scale) + ")";
    return out;
  } else if (!catalog::is_blob(t.kind)) {
    out += "(" + std::to_string(t.length) + ")";
  }
  if (catalog::is_numeric(t.kind)) {
    if (is_unsigned) out += " unsigned";
    if (t.zerofill) out += " zerofill";
  }
  return out;
}

std::optional<std::string> numeric_precision(const catalog::ColumnType& t) {
  switch (t.kind) {
    case TypeKind::tinyint:
      return "3";
    case TypeKind::smallint:
      return "5";
    case TypeKind::mediumint:
      return "7";
    case TypeKind::int_:
      return "10";
    case TypeKind::bigint:
      return "20";
    case TypeKind::decimal:
      return std::to_string(t.length);
    case TypeKind::float_:
      return std::to_string(t.length ? t.length : 12);
    case TypeKind::double_:
      return std::to_string(t.length ? t.length : 22);
    default:
      return std::nullopt;
  }
}

std::optional<std::string> numeric_scale(const catalog::ColumnType& t) {
  if (catalog::is_integer(t.kind)) return "0";
  if (t.kind == TypeKind::decimal) return std::to_string(t.scale);
  if ((t.kind == TypeKind::float_ || t.kind == TypeKind::double_) && t.length) return std::to_string(t.scale);
  return std::nullopt;
}

// PRI for a column of the primary key; UNI for the column of a one-column
// UNIQUE key; MUL for the first column of any other key; PRI before UNI
// before MUL (the manual's COLUMNS table, observed).
std::string column_key(const catalog::Table& table, size_t column) {
  if (const auto primary = table.primary_key())
    for (const auto& part : table.keys[*primary].parts)
      if (part.column == column) return "PRI";
  std::string best;
  for (const auto& key : table.keys) {
    if (key.parts.empty() || key.parts[0].column != column) continue;
    if (key.kind == catalog::Key::Kind::unique && key.parts.size() == 1) return "UNI";
    best = "MUL";
  }
  return best;
}

View schemata(const catalog::Catalog& catalog) {
  View v;
  v.table.name = "SCHEMATA";
  add(v, {"CATALOG_NAME", f::var_string, name_collation, 192, 0, nn | uk | bin | nd | pk});
  add(v, {"SCHEMA_NAME", f::var_string, name_collation, 192, 0, nn | bin | nd | pk});
  add(v, {"DEFAULT_CHARACTER_SET_NAME", f::var_string, text_collation, 192, 0, nn | uk | nd | pk});
  add(v, {"DEFAULT_COLLATION_NAME", f::var_string, text_collation, 192, 0, nn | uk | nd | pk});
  add(v, {"SQL_PATH", f::null, protocol::collation::binary, 0, 0, bin | num});
  add(v, {"DEFAULT_ENCRYPTION", f::string, name_collation, 9, 0, nn | bin | en | nd});
  v.rows.push_back({"def", std::string(information_schema_name), "utf8mb3", "utf8mb3_general_ci", std::nullopt, "NO"});
  for (const auto& name : catalog.database_names())
    if (const auto d = catalog.database(name)) v.rows.push_back({"def", name, d->charset, d->collation, std::nullopt, "NO"});
  return v;
}

View tables(const catalog::Catalog& catalog) {
  View v;
  v.table.name = "TABLES";
  const std::string temporal = "information_schema.TABLES' date and time columns";
  constexpr uint16_t binary = protocol::collation::binary;
  add(v, {"TABLE_CATALOG", f::var_string, name_collation, 192, 0, nn | uk | bin | nd | pk},
      {"TABLE_CATALOG", f::var_string, name_collation, 192, 0, nn | bin | nd});
  add(v, {"TABLE_SCHEMA", f::var_string, name_collation, 192, 0, nn | bin | nd | pk}, {"TABLE_SCHEMA", f::var_string, name_collation, 192, 0, nn | bin | nd});
  add(v, {"TABLE_NAME", f::var_string, name_collation, 192, 0, nn | bin | nd | pk}, {"TABLE_NAME", f::var_string, name_collation, 192, 0, nn | bin | nd});
  add(v, {"TABLE_TYPE", f::string, name_collation, 33, 0, nn | mk | bin | en | nd | pk}, {"TABLE_TYPE", f::string, name_collation, 33, 0, nn | bin | en | nd});
  add(v, {"ENGINE", f::var_string, text_collation, 192, 31, 0}, {"ENGINE", f::var_string, text_collation, 192, 0, 0});
  add(v, {"VERSION", f::longlong, binary, 3, 0, bin | num}, {"VERSION", f::long_, binary, 3, 0, num});
  add(v, {"ROW_FORMAT", f::string, name_collation, 30, 0, bin | en});
  for (const char* figure : {"TABLE_ROWS", "AVG_ROW_LENGTH", "DATA_LENGTH", "MAX_DATA_LENGTH", "INDEX_LENGTH", "DATA_FREE", "AUTO_INCREMENT"})
    add(v, {figure, f::longlong, binary, 21, 0, uns | bin | num}, {figure, f::longlong, binary, 21, 0, uns | num});
  add(v, {"CREATE_TIME", f::timestamp, binary, 19, 0, nn | bin | nd}, temporal);
  add(v, {"UPDATE_TIME", f::datetime, binary, 19, 0, bin}, temporal);
  add(v, {"CHECK_TIME", f::datetime, binary, 19, 0, bin}, temporal);
  add(v, {"TABLE_COLLATION", f::var_string, text_collation, 192, 0, uk | nd | pk}, {"TABLE_COLLATION", f::var_string, text_collation, 192, 0, nd});
  add(v, {"CHECKSUM", f::longlong, binary, 21, 0, bin | num}, {"CHECKSUM", f::longlong, binary, 21, 0, num});
  add(v, {"CREATE_OPTIONS", f::var_string, text_collation, 768, 31, 0}, {"CREATE_OPTIONS", f::var_string, text_collation, 768, 0, 0});
  add(v, {"TABLE_COMMENT", f::var_string, text_collation, 6144, 31, 0}, {"TABLE_COMMENT", f::blob, text_collation, 6144, 0, blb});
  for (const auto& database : catalog.database_names())
    for (const auto& name : catalog.table_names(database).value_or(std::vector<std::string>{})) {
      const auto t = catalog.table({database, name});
      if (!t) continue;
      txn::Row row{"def", database, name, "BASE TABLE", t->engine, "10", "Dynamic"};
      row.resize(row.size() + 7 + 3);  // the storage figures and the times: NULL
      row.push_back(t->collation);
      row.push_back(std::nullopt);
      row.push_back(text(""));
      row.push_back(t->comment);
      v.rows.push_back(std::move(row));
    }
  return v;
}

View columns(const catalog::Catalog& catalog) {
  View v;
  v.table.name = "COLUMNS";
  constexpr uint16_t binary = protocol::collation::binary;
  // Each column read directly, then through a temporary table.
  add(v, {"TABLE_CATALOG", f::var_string, name_collation, 192, 0, nn | uk | bin | nd | pk},
      {"TABLE_CATALOG", f::var_string, name_collation, 192, 0, nn | bin | nd});
  add(v, {"TABLE_SCHEMA", f::var_string, name_collation, 192, 0, nn | bin | nd | pk}, {"TABLE_SCHEMA", f::var_string, name_collation, 192, 0, nn | bin | nd});
  add(v, {"TABLE_NAME", f::var_string, name_collation, 192, 0, nn | bin | nd | pk}, {"TABLE_NAME", f::var_string, name_collation, 192, 0, nn | bin | nd});
  add(v, {"COLUMN_NAME", f::var_string, text_collation, 192, 31, 0}, {"COLUMN_NAME", f::var_string, text_collation, 192, 0, 0});
  add(v, {"ORDINAL_POSITION", f::long_, binary, 10, 0, nn | uns | nd | num | pk}, {"ORDINAL_POSITION", f::long_, binary, 10, 0, nn | uns | nd | num});
  add(v, {"COLUMN_DEFAULT", f::blob, name_collation, 65535, 0, blb | bin});
  add(v, {"IS_NULLABLE", f::var_string, text_collation, 9, 31, nn}, {"IS_NULLABLE", f::var_string, text_collation, 9, 0, nn});
  add(v, {"DATA_TYPE", f::long_blob, name_collation, 50331645, 31, bin}, {"DATA_TYPE", f::blob, name_collation, 50331645, 0, blb | bin});
  add(v, {"CHARACTER_MAXIMUM_LENGTH", f::longlong, binary, 21, 0, bin | num}, {"CHARACTER_MAXIMUM_LENGTH", f::longlong, binary, 21, 0, num});
  add(v, {"CHARACTER_OCTET_LENGTH", f::longlong, binary, 21, 0, bin | num}, {"CHARACTER_OCTET_LENGTH", f::longlong, binary, 21, 0, num});
  add(v, {"NUMERIC_PRECISION", f::long_, binary, 10, 0, uns | bin | num}, {"NUMERIC_PRECISION", f::longlong, binary, 10, 0, uns | num});
  add(v, {"NUMERIC_SCALE", f::long_, binary, 10, 0, uns | bin | num}, {"NUMERIC_SCALE", f::longlong, binary, 10, 0, uns | num});
  add(v, {"DATETIME_PRECISION", f::long_, binary, 10, 0, uns | num});
  add(v, {"CHARACTER_SET_NAME", f::var_string, text_collation, 192, 31, 0}, {"CHARACTER_SET_NAME", f::var_string, text_collation, 192, 0, 0});
  add(v, {"COLLATION_NAME", f::var_string, text_collation, 192, 31, 0}, {"COLLATION_NAME", f::var_string, text_collation, 192, 0, 0});
  add(v, {"COLUMN_TYPE", f::blob, name_collation, 16777215, 0, nn | blb | bin | nd});
  add(v, {"COLUMN_KEY", f::string, name_collation, 9, 0, nn | bin | en | nd});
  add(v, {"EXTRA", f::var_string, text_collation, 768, 31, 0}, {"EXTRA", f::var_string, text_collation, 768, 0, 0});
  add(v, {"PRIVILEGES", f::var_string, text_collation, 462, 31, 0}, {"PRIVILEGES", f::var_string, text_collation, 462, 0, 0});
  add(v, {"COLUMN_COMMENT", f::var_string, name_collation, 6144, 31, nn | bin}, {"COLUMN_COMMENT", f::blob, name_collation, 6144, 0, nn | blb | bin});
  add(v, {"GENERATION_EXPRESSION", f::blob, name_collation, 1073741824, 31, nn | bin},
      {"GENERATION_EXPRESSION", f::blob, name_collation, 1073741824, 0, nn | blb | bin});
  add(v, {"SRS_ID", f::long_, binary, 10, 0, mk | uns | num | pk}, {"SRS_ID", f::long_, binary, 10, 0, uns | num});
  for (const auto& database : catalog.database_names())
    for (const auto& name : catalog.table_names(database).value_or(std::vector<std::string>{})) {
      const auto t = catalog.table({database, name});
      if (!t) continue;
      for (size_t i = 0; i < t->columns.size(); ++i) {
        const auto& col = t->columns[i];
        const auto& type = col.type;
        const bool is_text = catalog::is_text(type.kind);
        std::optional<std::string> max_length, octets;
        if (catalog::is_blob(type.kind)) {
          max_length = octets = std::to_string(catalog::blob_max_bytes(type.kind));
        } else if (is_text) {
          max_length = std::to_string(type.length);
          octets = std::to_string(uint64_t{type.length} * catalog::max_bytes_per_character(type.charset));
        } else if (catalog::is_binary(type.kind)) {
          max_length = octets = std::to_string(type.length);
        }
        txn::Row row{"def", database, name, col.name, std::to_string(i + 1)};
        // CURRENT_TIMESTAMP with the column's fraction digits (observed:
        // CURRENT_TIMESTAMP(3)).
        const std::string now = "CURRENT_TIMESTAMP" + (type.scale ? "(" + std::to_string(type.scale) + ")" : std::string());
        if (col.default_kind == catalog::Column::Default::current_timestamp)
          row.push_back(now);
        else
          row.push_back(col.default_kind == catalog::Column::Default::value ? std::optional<std::string>(col.default_value) : std::nullopt);
        row.push_back(text(col.nullable ? "YES" : "NO"));
        row.push_back(kind_name(type.kind));
        row.push_back(max_length);
        row.push_back(octets);
        row.push_back(numeric_precision(type));
        row.push_back(numeric_scale(type));
        // DATETIME_PRECISION: TIME's, DATETIME's and TIMESTAMP's fraction
        // digits, 0 when none is written; NULL for the rest (observed).
        const bool fraction = catalog::is_temporal(type.kind) && type.kind != TypeKind::date;
        row.push_back(fraction ? std::optional<std::string>(std::to_string(type.scale)) : std::nullopt);
        row.push_back(is_text ? std::optional<std::string>(type.charset) : std::nullopt);
        row.push_back(is_text ? std::optional<std::string>(type.collation) : std::nullopt);
        row.push_back(column_type(type));
        row.push_back(column_key(*t, i));
        // EXTRA: auto_increment; DEFAULT_GENERATED for a clock default, and
        // "on update CURRENT_TIMESTAMP(n)" (observed).
        std::string extra = col.auto_increment ? "auto_increment" : "";
        if (col.default_kind == catalog::Column::Default::current_timestamp) extra = "DEFAULT_GENERATED";
        if (col.on_update) extra += (extra.empty() ? "" : " ") + std::string("on update ") + now;
        row.push_back(extra);
        row.push_back(text("select,insert,update,references"));
        row.push_back(col.comment);
        row.push_back(text(""));
        row.push_back(std::nullopt);
        v.rows.push_back(std::move(row));
      }
    }
  return v;
}

// MySQL 8.4.13's views (observed), upper case.
constexpr std::array<std::string_view, 79> mysql_views = {
    "ADMINISTRABLE_ROLE_AUTHORIZATIONS",
    "APPLICABLE_ROLES",
    "CHARACTER_SETS",
    "CHECK_CONSTRAINTS",
    "COLLATIONS",
    "COLLATION_CHARACTER_SET_APPLICABILITY",
    "COLUMNS",
    "COLUMNS_EXTENSIONS",
    "COLUMN_PRIVILEGES",
    "COLUMN_STATISTICS",
    "ENABLED_ROLES",
    "ENGINES",
    "EVENTS",
    "FILES",
    "INNODB_BUFFER_PAGE",
    "INNODB_BUFFER_PAGE_LRU",
    "INNODB_BUFFER_POOL_STATS",
    "INNODB_CACHED_INDEXES",
    "INNODB_CMP",
    "INNODB_CMPMEM",
    "INNODB_CMPMEM_RESET",
    "INNODB_CMP_PER_INDEX",
    "INNODB_CMP_PER_INDEX_RESET",
    "INNODB_CMP_RESET",
    "INNODB_COLUMNS",
    "INNODB_DATAFILES",
    "INNODB_FIELDS",
    "INNODB_FOREIGN",
    "INNODB_FOREIGN_COLS",
    "INNODB_FT_BEING_DELETED",
    "INNODB_FT_CONFIG",
    "INNODB_FT_DEFAULT_STOPWORD",
    "INNODB_FT_DELETED",
    "INNODB_FT_INDEX_CACHE",
    "INNODB_FT_INDEX_TABLE",
    "INNODB_INDEXES",
    "INNODB_METRICS",
    "INNODB_SESSION_TEMP_TABLESPACES",
    "INNODB_TABLES",
    "INNODB_TABLESPACES",
    "INNODB_TABLESPACES_BRIEF",
    "INNODB_TABLESTATS",
    "INNODB_TEMP_TABLE_INFO",
    "INNODB_TRX",
    "INNODB_VIRTUAL",
    "KEYWORDS",
    "KEY_COLUMN_USAGE",
    "OPTIMIZER_TRACE",
    "PARAMETERS",
    "PARTITIONS",
    "PLUGINS",
    "PROCESSLIST",
    "PROFILING",
    "REFERENTIAL_CONSTRAINTS",
    "RESOURCE_GROUPS",
    "ROLE_COLUMN_GRANTS",
    "ROLE_ROUTINE_GRANTS",
    "ROLE_TABLE_GRANTS",
    "ROUTINES",
    "SCHEMATA",
    "SCHEMATA_EXTENSIONS",
    "SCHEMA_PRIVILEGES",
    "STATISTICS",
    "ST_GEOMETRY_COLUMNS",
    "ST_SPATIAL_REFERENCE_SYSTEMS",
    "ST_UNITS_OF_MEASURE",
    "TABLES",
    "TABLESPACES_EXTENSIONS",
    "TABLES_EXTENSIONS",
    "TABLE_CONSTRAINTS",
    "TABLE_CONSTRAINTS_EXTENSIONS",
    "TABLE_PRIVILEGES",
    "TRIGGERS",
    "USER_ATTRIBUTES",
    "USER_PRIVILEGES",
    "VIEWS",
    "VIEW_ROUTINE_USAGE",
    "VIEW_TABLE_USAGE",
};

}  // namespace

bool is_information_schema(std::string_view database) { return sql::same_word(database, information_schema_name); }

View information_schema_view(const std::string& name, const catalog::Catalog& catalog) {
  if (sql::same_word(name, "SCHEMATA")) return schemata(catalog);
  if (sql::same_word(name, "TABLES")) return tables(catalog);
  if (sql::same_word(name, "COLUMNS")) return columns(catalog);
  const auto upper = sql::upper(name);
  if (std::find(mysql_views.begin(), mysql_views.end(), upper) != mysql_views.end())
    throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'information_schema." + upper + "'");
  throw SqlError(1109, "42S02", "Unknown table '" + name + "' in information_schema");
}

}  // namespace kvasir::exec
