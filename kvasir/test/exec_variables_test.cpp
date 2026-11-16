//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual, "Server System Variables" and "Using System Variables";
// the defaults, types, normal forms, clamping and refusals below were
// observed on MySQL 8.4.13 (Oracle's container on node-b,
// 2026-11-15/16). ack_level is Kvasir's (the design, rulings 11 and 23).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kvasir/exec/variables.h"
#include "kvasir/protocol/types.h"

namespace kvasir::exec {
namespace {

const VariableRegistry& registry() {
  static const VariableRegistry r("replicated", "8.4.13-kvasir", "Kvasir - The Distributed Relational Database (in memory, not durable)");
  return r;
}

std::string text_of(const Value& v) {
  if (const auto* s = std::get_if<std::string>(&v)) return *s;
  if (const auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
  if (const auto* u = std::get_if<uint64_t>(&v)) return std::to_string(*u);
  return "NULL";
}

struct Refusal {
  uint16_t code = 0;
  std::string sqlstate;
  std::string message;
};

template <typename F>
Refusal refusal(F f) {
  try {
    f();
  } catch (const SqlError& e) {
    return {e.code(), e.sqlstate(), e.what()};
  }
  return {};
}

TEST(ExecVariables, DefaultsAreMySqls) {
  SessionVariables v(registry());
  EXPECT_EQ(text_of(v.get("", "autocommit")), "1");
  EXPECT_EQ(text_of(v.get("", "sql_mode")),
            "ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION");
  EXPECT_EQ(text_of(v.get("", "max_allowed_packet")), "67108864");
  EXPECT_EQ(text_of(v.get("", "character_set_client")), "utf8mb4");
  EXPECT_EQ(text_of(v.get("", "collation_connection")), "utf8mb4_0900_ai_ci");
  EXPECT_EQ(text_of(v.get("", "transaction_isolation")), "REPEATABLE-READ");
  EXPECT_EQ(text_of(v.get("", "auto_increment_increment")), "1");
  EXPECT_EQ(text_of(v.get("", "wait_timeout")), "28800");
  EXPECT_EQ(text_of(v.get("", "div_precision_increment")), "4");
  EXPECT_EQ(text_of(v.get("", "sql_select_limit")), "18446744073709551615");
  EXPECT_EQ(text_of(v.get("", "time_zone")), "SYSTEM");
  EXPECT_EQ(text_of(v.get("", "init_connect")), "");
  EXPECT_EQ(text_of(v.get("", "protocol_version")), "10");
  EXPECT_EQ(text_of(v.get("", "version")), "8.4.13-kvasir");
  EXPECT_EQ(text_of(v.get("", "version_comment")), "Kvasir - The Distributed Relational Database (in memory, not durable)");
  EXPECT_EQ(text_of(v.get("", "ack_level")), "replicated");
}

TEST(ExecVariables, NamesIgnoreCase) {
  SessionVariables v(registry());
  EXPECT_EQ(text_of(v.get("", "AutoCommit")), "1");
  EXPECT_EQ(text_of(v.get("SESSION", "SQL_SELECT_LIMIT")), "18446744073709551615");
}

TEST(ExecVariables, TypesAreMySqls) {
  SessionVariables v(registry());
  namespace f = protocol::field_type;
  namespace c = protocol::column_flag;
  const auto text = v.type_of("", "version_comment", 255, 4);
  EXPECT_EQ(text.field_type, f::var_string);
  EXPECT_EQ(text.collation, 255);
  EXPECT_EQ(text.length, 87380u);
  EXPECT_EQ(text.decimals, 31);
  EXPECT_EQ(text.flags, 0);
  EXPECT_EQ(v.type_of("", "sql_mode", 46, 4).collation, 46);
  const auto boolean = v.type_of("", "autocommit", 255, 4);
  EXPECT_EQ(boolean.field_type, f::longlong);
  EXPECT_EQ(boolean.length, 1u);
  EXPECT_EQ(boolean.decimals, 0);
  EXPECT_EQ(boolean.flags, c::binary | c::num);
  const auto number = v.type_of("", "max_allowed_packet", 255, 4);
  EXPECT_EQ(number.field_type, f::longlong);
  EXPECT_EQ(number.length, 21u);
  EXPECT_EQ(number.flags, c::unsigned_ | c::binary | c::num);
}

TEST(ExecVariables, UnknownNamesAndScopesAreRefused) {
  SessionVariables v(registry());
  auto r = refusal([&] { v.get("", "nope"); });
  EXPECT_EQ(r.code, 1193);
  EXPECT_EQ(r.sqlstate, "HY000");
  EXPECT_EQ(r.message, "Unknown system variable 'nope'");
  r = refusal([&] { v.get("SESSION", "version_comment"); });
  EXPECT_EQ(r.code, 1238);
  EXPECT_EQ(r.message, "Variable 'version_comment' is a GLOBAL variable");
  EXPECT_EQ(text_of(v.get("GLOBAL", "autocommit")), "1");
  EXPECT_EQ(text_of(v.get("LOCAL", "autocommit")), "1");
}

TEST(ExecVariables, BooleansTakeTheirWords) {
  SessionVariables v(registry());
  std::vector<Warning> w;
  v.set("", "autocommit", int64_t{0}, false, w);
  EXPECT_EQ(text_of(v.get("", "autocommit")), "0");
  v.set("", "autocommit", std::string("ON"), false, w);
  EXPECT_EQ(text_of(v.get("", "autocommit")), "1");
  v.set("", "autocommit", std::string("off"), false, w);
  EXPECT_EQ(text_of(v.get("", "autocommit")), "0");
  v.set("", "autocommit", std::string("true"), false, w);
  EXPECT_EQ(text_of(v.get("", "autocommit")), "1");
  EXPECT_TRUE(w.empty());

  auto r = refusal([&] { v.set("", "autocommit", int64_t{2}, false, w); });
  EXPECT_EQ(r.code, 1231);
  EXPECT_EQ(r.sqlstate, "42000");
  EXPECT_EQ(r.message, "Variable 'autocommit' can't be set to the value of '2'");
  r = refusal([&] { v.set("", "autocommit", std::string("x"), false, w); });
  EXPECT_EQ(r.message, "Variable 'autocommit' can't be set to the value of 'x'");
  r = refusal([&] { v.set("", "autocommit", *Decimal::parse("1.5"), false, w); });
  EXPECT_EQ(r.code, 1232);
  EXPECT_EQ(r.sqlstate, "42000");
  EXPECT_EQ(r.message, "Incorrect argument type to variable 'autocommit'");
}

TEST(ExecVariables, ReadOnlyGlobalOnlyAndUnknownAreRefused) {
  SessionVariables v(registry());
  std::vector<Warning> w;
  auto r = refusal([&] { v.set("", "version_comment", std::string("x"), false, w); });
  EXPECT_EQ(r.code, 1238);
  EXPECT_EQ(r.sqlstate, "HY000");
  EXPECT_EQ(r.message, "Variable 'version_comment' is a read only variable");
  r = refusal([&] { v.set("", "max_allowed_packet", int64_t{1024}, false, w); });
  EXPECT_EQ(r.code, 1621);
  EXPECT_EQ(r.message, "SESSION variable 'max_allowed_packet' is read-only. Use SET GLOBAL to assign the value");
  r = refusal([&] { v.set("", "nope", int64_t{1}, false, w); });
  EXPECT_EQ(r.code, 1193);
  // A global value is the cluster's, which SET GLOBAL cannot change alone.
  EXPECT_EQ(refusal([&] { v.set("GLOBAL", "wait_timeout", int64_t{100}, false, w); }).code, 1235);
  EXPECT_EQ(refusal([&] { v.set("PERSIST", "wait_timeout", int64_t{100}, false, w); }).code, 1235);
}

TEST(ExecVariables, NumbersAreClampedWithAWarning) {
  SessionVariables v(registry());
  std::vector<Warning> w;
  v.set("", "wait_timeout", int64_t{0}, false, w);
  EXPECT_EQ(text_of(v.get("", "wait_timeout")), "1");
  ASSERT_EQ(w.size(), 1u);
  EXPECT_EQ(w[0].level, "Warning");
  EXPECT_EQ(w[0].code, 1292);
  EXPECT_EQ(w[0].message, "Truncated incorrect wait_timeout value: '0'");
  w.clear();
  v.set("", "wait_timeout", int64_t{100}, false, w);
  EXPECT_EQ(text_of(v.get("", "wait_timeout")), "100");
  EXPECT_TRUE(w.empty());
  EXPECT_EQ(refusal([&] { v.set("", "wait_timeout", std::string("x"), false, w); }).code, 1232);
}

TEST(ExecVariables, SqlModeTakesMySqlsNormalForm) {
  SessionVariables v(registry());
  std::vector<Warning> w;
  const auto mode = [&](const Value& value) {
    v.set("", "sql_mode", value, false, w);
    return text_of(v.get("", "sql_mode"));
  };
  EXPECT_EQ(mode(std::string("ansi_quotes,real_as_float")), "REAL_AS_FLOAT,ANSI_QUOTES");
  EXPECT_EQ(mode(std::string("ANSI")), "REAL_AS_FLOAT,PIPES_AS_CONCAT,ANSI_QUOTES,IGNORE_SPACE,ONLY_FULL_GROUP_BY,ANSI");
  EXPECT_EQ(mode(std::string("TRADITIONAL")),
            "STRICT_TRANS_TABLES,STRICT_ALL_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,TRADITIONAL,NO_ENGINE_SUBSTITUTION");
  EXPECT_EQ(mode(std::string("")), "");
  EXPECT_EQ(mode(int64_t{3}), "REAL_AS_FLOAT,PIPES_AS_CONCAT");
  v.set("", "sql_mode", {}, true, w);
  EXPECT_EQ(text_of(v.get("", "sql_mode")),
            "ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION");
  const auto r = refusal([&] { v.set("", "sql_mode", std::string("NOPE"), false, w); });
  EXPECT_EQ(r.code, 1231);
  EXPECT_EQ(r.message, "Variable 'sql_mode' can't be set to the value of 'NOPE'");
}

TEST(ExecVariables, EnumerationsTakeTheirChoices) {
  SessionVariables v(registry());
  std::vector<Warning> w;
  v.set("", "transaction_isolation", std::string("read-committed"), false, w);
  EXPECT_EQ(text_of(v.get("", "transaction_isolation")), "READ-COMMITTED");
  EXPECT_EQ(refusal([&] { v.set("", "transaction_isolation", std::string("nope"), false, w); }).message,
            "Variable 'transaction_isolation' can't be set to the value of 'nope'");
  v.set("", "ack_level", std::string("DECIDED"), false, w);
  EXPECT_EQ(text_of(v.get("", "ack_level")), "decided");
  EXPECT_EQ(refusal([&] { v.set("", "ack_level", std::string("durable"), false, w); }).code, 1231);
}

TEST(ExecVariables, CharacterSetsAndCollations) {
  SessionVariables v(registry());
  std::vector<Warning> w;
  v.set("", "collation_connection", std::string("utf8mb4_bin"), false, w);
  EXPECT_EQ(text_of(v.get("", "collation_connection")), "utf8mb4_bin");
  auto r = refusal([&] { v.set("", "collation_connection", std::string("nope"), false, w); });
  EXPECT_EQ(r.code, 1273);
  EXPECT_EQ(r.message, "Unknown collation: 'nope'");
  r = refusal([&] { v.set("", "character_set_client", std::string("nope"), false, w); });
  EXPECT_EQ(r.code, 1115);
  EXPECT_EQ(r.sqlstate, "42000");
  EXPECT_EQ(r.message, "Unknown character set: 'nope'");
  // A character set MySQL has and Kvasir does not yet convert to.
  EXPECT_EQ(refusal([&] { v.set("", "character_set_client", std::string("latin1"), false, w); }).code, 1235);
  v.set("", "character_set_results", {}, false, w);
  EXPECT_EQ(text_of(v.get("", "character_set_results")), "NULL");
  EXPECT_EQ(refusal([&] { v.set("", "character_set_client", Value{}, false, w); }).code, 1231);
  ASSERT_TRUE(find_collation("utf8mb4_0900_ai_ci"));
  EXPECT_EQ(find_collation("utf8mb4_0900_ai_ci")->id, 255);
  EXPECT_EQ(find_collation("UTF8MB4_BIN")->id, 46);
  EXPECT_EQ(default_collation("utf8mb4")->name, "utf8mb4_0900_ai_ci");
  EXPECT_EQ(default_collation("binary")->id, 63);
}

TEST(ExecVariables, DefaultIsTheGlobalValue) {
  SessionVariables v(registry());
  std::vector<Warning> w;
  v.set("", "wait_timeout", int64_t{100}, false, w);
  v.set("", "wait_timeout", {}, true, w);
  EXPECT_EQ(text_of(v.get("", "wait_timeout")), "28800");
}

}  // namespace
}  // namespace kvasir::exec
