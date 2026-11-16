//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/exec/variables.h"

#include <algorithm>
#include <array>
#include <cctype>

#include "kvasir/protocol/types.h"
#include "kvasir/sql/keywords.h"

namespace kvasir::exec {

namespace {

namespace f = protocol::field_type;
namespace c = protocol::column_flag;

std::string lower(std::string_view s) {
  std::string out(s);
  for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

// sql_mode's names in bit order; empty for bits not used.
constexpr std::array<std::string_view, 33> sql_mode_names = {
    "REAL_AS_FLOAT",
    "PIPES_AS_CONCAT",
    "ANSI_QUOTES",
    "IGNORE_SPACE",
    "",
    "ONLY_FULL_GROUP_BY",
    "NO_UNSIGNED_SUBTRACTION",
    "NO_DIR_IN_CREATE",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "ANSI",
    "NO_AUTO_VALUE_ON_ZERO",
    "NO_BACKSLASH_ESCAPES",
    "STRICT_TRANS_TABLES",
    "STRICT_ALL_TABLES",
    "NO_ZERO_IN_DATE",
    "NO_ZERO_DATE",
    "ALLOW_INVALID_DATES",
    "ERROR_FOR_DIVISION_BY_ZERO",
    "TRADITIONAL",
    "",
    "HIGH_NOT_PRECEDENCE",
    "NO_ENGINE_SUBSTITUTION",
    "PAD_CHAR_TO_FULL_LENGTH",
    "TIME_TRUNCATE_FRACTIONAL",
};

uint64_t mode_bit(std::string_view name) {
  for (size_t i = 0; i < sql_mode_names.size(); ++i)
    if (!sql_mode_names[i].empty() && sql::same_word(sql_mode_names[i], name)) return uint64_t{1} << i;
  return 0;
}

// The combination modes switch on their members.
uint64_t expand(uint64_t mode) {
  const auto bit = [](std::string_view n) { return mode_bit(n); };
  if (mode & bit("ANSI")) mode |= bit("REAL_AS_FLOAT") | bit("PIPES_AS_CONCAT") | bit("ANSI_QUOTES") | bit("IGNORE_SPACE") | bit("ONLY_FULL_GROUP_BY");
  if (mode & bit("TRADITIONAL"))
    mode |= bit("STRICT_TRANS_TABLES") | bit("STRICT_ALL_TABLES") | bit("NO_ZERO_IN_DATE") | bit("NO_ZERO_DATE") | bit("ERROR_FOR_DIVISION_BY_ZERO") |
            bit("NO_ENGINE_SUBSTITUTION");
  return mode;
}

std::string mode_text(uint64_t mode) {
  std::string out;
  for (size_t i = 0; i < sql_mode_names.size(); ++i)
    if ((mode >> i) & 1) out += (out.empty() ? "" : ",") + std::string(sql_mode_names[i]);
  return out;
}

const std::array<CollationInfo, 7> collations = {{
    {"utf8mb4_0900_ai_ci", 255, "utf8mb4", true},
    {"utf8mb4_bin", 46, "utf8mb4", false},
    {"utf8mb4_general_ci", 45, "utf8mb4", false},
    {"utf8mb4_unicode_ci", 224, "utf8mb4", false},
    {"utf8mb4_0900_as_cs", 278, "utf8mb4", false},
    {"utf8mb4_0900_bin", 309, "utf8mb4", false},
    {"binary", 63, "binary", true},
}};

// Character sets MySQL has that Kvasir does not yet convert to.
constexpr std::array<std::string_view, 12> other_charsets = {"latin1",  "utf8mb3", "utf8",   "ascii",  "ucs2",   "utf16",
                                                             "utf16le", "utf32",   "latin2", "cp1250", "cp1251", "gbk"};

std::string value_text(const Value& v) {
  if (const auto* s = std::get_if<std::string>(&v)) return *s;
  if (const auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
  if (const auto* u = std::get_if<uint64_t>(&v)) return std::to_string(*u);
  if (const auto* d = std::get_if<Decimal>(&v)) return d->to_string();
  if (const auto* r = std::get_if<double>(&v)) return format_double(*r);
  return "NULL";
}

[[noreturn]] void cannot_take(const std::string& name, const Value& v) {
  throw SqlError(1231, "42000", "Variable '" + name + "' can't be set to the value of '" + value_text(v) + "'");
}
[[noreturn]] void wrong_type(const std::string& name) { throw SqlError(1232, "42000", "Incorrect argument type to variable '" + name + "'"); }

VariableSpec boolean(std::string name, bool initial, bool has_session = true) {
  VariableSpec s;
  s.name = std::move(name);
  s.type = VariableType::boolean;
  s.initial = int64_t{initial ? 1 : 0};
  s.has_session = has_session;
  return s;
}

VariableSpec number(std::string name, uint64_t initial, uint64_t minimum, uint64_t maximum, bool has_session = true) {
  VariableSpec s;
  s.name = std::move(name);
  s.type = VariableType::unsigned_;
  s.initial = initial;
  s.minimum = minimum;
  s.maximum = maximum;
  s.has_session = has_session;
  return s;
}

VariableSpec text(std::string name, std::string initial, VariableType type = VariableType::string, bool has_session = true) {
  VariableSpec s;
  s.name = std::move(name);
  s.type = type;
  s.initial = std::move(initial);
  s.has_session = has_session;
  return s;
}

VariableSpec read_only(VariableSpec s) {
  s.read_only = true;
  s.has_session = false;
  return s;
}

}  // namespace

const CollationInfo* find_collation(std::string_view name) {
  for (const auto& c : collations)
    if (sql::same_word(c.name, name)) return &c;
  return nullptr;
}

const CollationInfo* default_collation(std::string_view charset) {
  for (const auto& c : collations)
    if (c.is_default && sql::same_word(c.charset, charset)) return &c;
  return nullptr;
}

VariableRegistry::VariableRegistry(std::string ack_level, std::string version, std::string version_comment) {
  constexpr uint64_t year = 31536000;
  _specs.push_back(boolean("autocommit", true));
  _specs.push_back(text("sql_mode", "ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION",
                        VariableType::sql_mode));
  auto packet = number("max_allowed_packet", 67108864, 1024, 1073741824);
  packet.session_read_only = true;
  _specs.push_back(packet);
  auto buffer = number("net_buffer_length", 16384, 1024, 1048576);
  buffer.session_read_only = true;
  _specs.push_back(buffer);
  for (const char* name : {"character_set_client", "character_set_connection", "character_set_results", "character_set_server"}) {
    auto s = text(name, "utf8mb4", VariableType::charset);
    s.nullable = std::string(name) == "character_set_results";
    _specs.push_back(s);
  }
  _specs.push_back(text("collation_connection", "utf8mb4_0900_ai_ci", VariableType::collation));
  _specs.push_back(text("collation_server", "utf8mb4_0900_ai_ci", VariableType::collation));
  _specs.push_back(number("auto_increment_increment", 1, 1, 65535));
  _specs.push_back(number("auto_increment_offset", 1, 1, 65535));
  auto isolation = text("transaction_isolation", "REPEATABLE-READ", VariableType::enumeration);
  isolation.choices = {"READ-UNCOMMITTED", "READ-COMMITTED", "REPEATABLE-READ", "SERIALIZABLE"};
  _specs.push_back(isolation);
  _specs.push_back(boolean("transaction_read_only", false));
  _specs.push_back(read_only(number("lower_case_table_names", 0, 0, 2)));
  _specs.push_back(text("time_zone", "SYSTEM"));
  _specs.push_back(read_only(text("system_time_zone", "UTC")));
  _specs.push_back(number("wait_timeout", 28800, 1, year));
  _specs.push_back(number("interactive_timeout", 28800, 1, year));
  _specs.push_back(number("net_write_timeout", 60, 1, year));
  _specs.push_back(number("net_read_timeout", 30, 1, year));
  _specs.push_back(read_only(boolean("performance_schema", false)));
  _specs.push_back(read_only(text("license", "GPL")));
  _specs.push_back(text("init_connect", "", VariableType::string, false));
  _specs.push_back(number("div_precision_increment", 4, 0, 30));
  _specs.push_back(number("innodb_lock_wait_timeout", 50, 1, 1073741824));
  _specs.push_back(number("max_execution_time", 0, 0, UINT32_MAX));
  _specs.push_back(number("sql_select_limit", UINT64_MAX, 0, UINT64_MAX));
  _specs.push_back(boolean("sql_auto_is_null", false));
  _specs.push_back(boolean("sql_safe_updates", false));
  _specs.push_back(boolean("foreign_key_checks", true));
  _specs.push_back(boolean("unique_checks", true));
  _specs.push_back(boolean("sql_notes", true));
  _specs.push_back(boolean("sql_warnings", false));
  _specs.push_back(read_only(number("protocol_version", 10, 0, 255)));
  _specs.push_back(read_only(text("version", std::move(version))));
  _specs.push_back(read_only(text("version_comment", std::move(version_comment))));
  _specs.push_back(read_only(text("version_compile_os", "Linux")));
#if defined(__aarch64__)
  _specs.push_back(read_only(text("version_compile_machine", "aarch64")));
#else
  _specs.push_back(read_only(text("version_compile_machine", "x86_64")));
#endif
  auto ack = text("ack_level", std::move(ack_level), VariableType::enumeration);
  ack.choices = {"ephemeral", "persisted", "decided", "replicated"};
  _specs.push_back(ack);
}

const VariableSpec* VariableRegistry::find(std::string_view name) const {
  for (const auto& s : _specs)
    if (sql::same_word(s.name, name)) return &s;
  return nullptr;
}

SessionVariables::SessionVariables(const VariableRegistry& registry) : _registry(registry) { reset(); }

void SessionVariables::reset() {
  _global.clear();
  _session.clear();
  for (const auto& s : _registry.all()) {
    _global[s.name] = s.initial;
    if (s.has_session) _session[s.name] = s.initial;
  }
}

const VariableSpec& SessionVariables::spec(std::string_view name) const {
  const auto* s = _registry.find(name);
  if (!s) throw SqlError(1193, "HY000", "Unknown system variable '" + std::string(name) + "'");
  return *s;
}

Value SessionVariables::get(std::string_view scope, std::string_view name) const {
  const auto& s = spec(name);
  if (sql::same_word(scope, "GLOBAL")) return _global.at(s.name);
  if (!scope.empty() && !s.has_session) throw SqlError(1238, "HY000", "Variable '" + s.name + "' is a GLOBAL variable");
  if (!s.has_session) return _global.at(s.name);
  return _session.at(s.name);
}

ResultType SessionVariables::type_of(std::string_view scope, std::string_view name, uint16_t result_collation, uint32_t result_max_bytes) const {
  const auto& s = spec(name);
  get(scope, name);
  ResultType t;
  switch (s.type) {
    case VariableType::boolean:
      t.field_type = f::longlong;
      t.collation = protocol::collation::binary;
      t.length = 1;
      t.precision = 1;
      t.flags = c::binary | c::num;
      return t;
    case VariableType::unsigned_:
      t.field_type = f::longlong;
      t.collation = protocol::collation::binary;
      t.length = 21;
      t.precision = 21;
      t.flags = c::unsigned_ | c::binary | c::num;
      return t;
    default:
      t.field_type = f::var_string;
      t.collation = result_collation;
      t.length = 21845 * result_max_bytes;
      t.decimals = protocol::not_fixed_decimals;
      return t;
  }
}

void SessionVariables::set(std::string_view scope, std::string_view name, const Value& value, bool use_default, std::vector<Warning>& warnings) {
  const auto& s = spec(name);
  if (s.read_only) throw SqlError(1238, "HY000", "Variable '" + s.name + "' is a read only variable");
  if (sql::same_word(scope, "GLOBAL") || sql::same_word(scope, "PERSIST") || sql::same_word(scope, "PERSIST_ONLY"))
    throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'SET " + sql::upper(scope) + "'");
  if (!s.has_session) throw SqlError(1229, "HY000", "Variable '" + s.name + "' is a GLOBAL variable and should be set with SET GLOBAL");
  if (s.session_read_only) throw SqlError(1621, "HY000", "SESSION variable '" + s.name + "' is read-only. Use SET GLOBAL to assign the value");
  if (use_default) {
    _session[s.name] = _global.at(s.name);
    return;
  }
  const auto* str = std::get_if<std::string>(&value);
  const bool integer = std::holds_alternative<int64_t>(value) || std::holds_alternative<uint64_t>(value);
  Value result;
  switch (s.type) {
    case VariableType::boolean: {
      if (is_null(value)) cannot_take(s.name, value);
      if (integer) {
        const auto* i = std::get_if<int64_t>(&value);
        const auto* u = std::get_if<uint64_t>(&value);
        const bool zero = i ? *i == 0 : *u == 0;
        const bool one = i ? *i == 1 : *u == 1;
        if (!zero && !one) cannot_take(s.name, value);
        result = int64_t{one ? 1 : 0};
      } else if (str) {
        if (sql::same_word(*str, "ON") || sql::same_word(*str, "TRUE"))
          result = int64_t{1};
        else if (sql::same_word(*str, "OFF") || sql::same_word(*str, "FALSE"))
          result = int64_t{0};
        else
          cannot_take(s.name, value);
      } else {
        wrong_type(s.name);
      }
      break;
    }
    case VariableType::unsigned_: {
      if (!integer) wrong_type(s.name);
      uint64_t v = 0;
      bool clamped = false;
      if (const auto* i = std::get_if<int64_t>(&value)) {
        if (*i < 0) {
          v = s.minimum;
          clamped = true;
        } else {
          v = static_cast<uint64_t>(*i);
        }
      } else {
        v = std::get<uint64_t>(value);
      }
      if (v < s.minimum) v = s.minimum, clamped = true;
      if (v > s.maximum) v = s.maximum, clamped = true;
      if (clamped) warnings.push_back({"Warning", er_truncated_wrong_value, "Truncated incorrect " + s.name + " value: '" + value_text(value) + "'"});
      result = v;
      break;
    }
    case VariableType::string:
      if (!str) wrong_type(s.name);
      if (s.name == "time_zone") {
        const auto& z = *str;
        const bool offset = z.size() == 6 && (z[0] == '+' || z[0] == '-') && std::isdigit(static_cast<unsigned char>(z[1])) &&
                            std::isdigit(static_cast<unsigned char>(z[2])) && z[3] == ':' && std::isdigit(static_cast<unsigned char>(z[4])) &&
                            std::isdigit(static_cast<unsigned char>(z[5]));
        if (!offset && !sql::same_word(z, "SYSTEM")) throw SqlError(1298, "HY000", "Unknown or incorrect time zone: '" + z + "'");
        result = sql::same_word(z, "SYSTEM") ? std::string("SYSTEM") : z;
      } else {
        result = *str;
      }
      break;
    case VariableType::enumeration: {
      if (!str) cannot_take(s.name, value);
      const auto it = std::find_if(s.choices.begin(), s.choices.end(), [&](const std::string& c) { return sql::same_word(c, *str); });
      if (it == s.choices.end()) cannot_take(s.name, value);
      result = *it;
      break;
    }
    case VariableType::sql_mode: {
      uint64_t mode = 0;
      if (integer) {
        const auto* i = std::get_if<int64_t>(&value);
        mode = i ? static_cast<uint64_t>(*i) : std::get<uint64_t>(value);
        if (mode >> sql_mode_names.size()) cannot_take(s.name, value);
        for (size_t b = 0; b < sql_mode_names.size(); ++b)
          if (((mode >> b) & 1) && sql_mode_names[b].empty()) cannot_take(s.name, value);
      } else if (str) {
        size_t start = 0;
        while (start <= str->size() && !str->empty()) {
          const auto comma = str->find(',', start);
          const auto part = std::string_view(*str).substr(start, comma == std::string::npos ? std::string::npos : comma - start);
          if (!part.empty()) {
            const auto bit = mode_bit(part);
            if (!bit) cannot_take(s.name, value);
            mode |= bit;
          }
          if (comma == std::string::npos) break;
          start = comma + 1;
        }
      } else {
        wrong_type(s.name);
      }
      result = mode_text(expand(mode));
      break;
    }
    case VariableType::charset: {
      if (is_null(value)) {
        if (!s.nullable) cannot_take(s.name, value);
        break;
      }
      if (!str) wrong_type(s.name);
      if (default_collation(*str)) {
        result = lower(*str);
      } else if (std::find_if(other_charsets.begin(), other_charsets.end(), [&](std::string_view c) { return sql::same_word(c, *str); }) !=
                 other_charsets.end()) {
        throw SqlError(er_not_supported_yet, "42000", "This version of Kvasir doesn't yet support 'character set " + lower(*str) + "'");
      } else {
        throw SqlError(1115, "42000", "Unknown character set: '" + *str + "'");
      }
      break;
    }
    case VariableType::collation: {
      if (!str) wrong_type(s.name);
      const auto* collation = find_collation(*str);
      if (!collation) throw SqlError(1273, "HY000", "Unknown collation: '" + *str + "'");
      result = collation->name;
      break;
    }
  }
  _session[s.name] = result;
}

std::string SessionVariables::text(std::string_view name) const { return value_text(get("", name)); }

uint64_t SessionVariables::number(std::string_view name) const {
  const auto v = get("", name);
  if (const auto* u = std::get_if<uint64_t>(&v)) return *u;
  if (const auto* i = std::get_if<int64_t>(&v)) return static_cast<uint64_t>(*i);
  return 0;
}

}  // namespace kvasir::exec
