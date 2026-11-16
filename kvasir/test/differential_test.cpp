//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The differential gate, recorded: each statement in
// kvasir/test/differential/*.sql was run on the reference MySQL 8.4 server by
// tools/record_mysql.sh, and its columns (name, type, collation, length,
// decimals, flags), rows and error are in the matching .expected file. Here
// each is run through one Kvasir session in order and must answer the same,
// up to the allowed differences the .sql file lists. Re-record when the list
// grows; never edit an .expected file by hand.

#include <gtest/gtest.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "kvasir/exec/session.h"
#include "kvasir/protocol/types.h"
#include "kvasir/sql/parser.h"

#ifndef KVASIR_DIFFERENTIAL_DIR
#error "KVASIR_DIFFERENTIAL_DIR names kvasir/test/differential"
#endif

namespace kvasir {
namespace {

// As the mysql client names them.
std::string type_name(uint8_t t) {
  static const std::vector<std::pair<uint8_t, const char*>> names = {
      {0, "DECIMAL"},       {1, "TINY"},        {2, "SHORT"},  {3, "LONG"},         {4, "FLOAT"},    {5, "DOUBLE"},     {6, "NULL"},
      {7, "TIMESTAMP"},     {8, "LONGLONG"},    {9, "INT24"},  {10, "DATE"},        {11, "TIME"},    {12, "DATETIME"},  {13, "YEAR"},
      {15, "VARCHAR"},      {16, "BIT"},        {245, "JSON"}, {246, "NEWDECIMAL"}, {247, "ENUM"},   {248, "SET"},      {249, "TINY_BLOB"},
      {250, "MEDIUM_BLOB"}, {251, "LONG_BLOB"}, {252, "BLOB"}, {253, "VAR_STRING"}, {254, "STRING"}, {255, "GEOMETRY"},
  };
  for (const auto& [code, name] : names)
    if (code == t) return name;
  return "?" + std::to_string(t);
}

std::string flag_names(uint16_t flags) {
  namespace c = protocol::column_flag;
  static const std::vector<std::pair<uint16_t, const char*>> names = {
      {c::not_null, "NOT_NULL"},
      {c::primary_key, "PRI_KEY"},
      {c::unique_key, "UNIQUE_KEY"},
      {c::multiple_key, "MULTIPLE_KEY"},
      {c::blob, "BLOB"},
      {c::unsigned_, "UNSIGNED"},
      {c::zerofill, "ZEROFILL"},
      {c::binary, "BINARY"},
      {c::enum_, "ENUM"},
      {c::auto_increment, "AUTO_INCREMENT"},
      {c::timestamp, "TIMESTAMP"},
      {c::set, "SET"},
      {c::no_default_value, "NO_DEFAULT_VALUE"},
      {c::on_update_now, "ON_UPDATE_NOW"},
      {c::num, "NUM"},
      {c::part_key, "PART_KEY"},
  };
  std::string out;
  for (const auto& [bit, name] : names)
    if (flags & bit) out += std::string(out.empty() ? "" : " ") + name;
  return out;
}

std::string trimmed(const std::string& s) {
  const auto first = s.find_first_not_of(' ');
  if (first == std::string::npos) return "";
  return s.substr(first, s.find_last_not_of(' ') - first + 1);
}

struct Case {
  std::string statement;
  std::vector<std::string> answer;  // the column, row and error lines, as recorded
};

std::vector<Case> load(const std::string& name) {
  std::ifstream in(std::string(KVASIR_DIFFERENTIAL_DIR) + "/" + name);
  EXPECT_TRUE(in) << "cannot read " << name;
  std::vector<Case> cases;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("statement ", 0) == 0) {
      cases.push_back({line.substr(10), {}});
    } else if (line != "end" && !cases.empty()) {
      cases.back().answer.push_back(line);
    }
  }
  return cases;
}

// Kvasir's answer, in the recorder's lines.
std::vector<std::string> answer(exec::Session& session, const std::string& statement) {
  std::vector<std::string> out;
  try {
    const auto statements = sql::parse(statement, session.parser_options());
    const auto outcome = session.execute(statements.at(0));
    if (!outcome.result) return out;
    for (const auto& c : outcome.result->columns)
      out.push_back("column " + c.name + "|" + type_name(c.type.field_type) + "|" + std::to_string(c.type.collation) + "|" + std::to_string(c.type.length) +
                    "|" + std::to_string(c.type.decimals) + "|" + flag_names(c.type.flags));
    for (const auto& row : outcome.result->rows) {
      std::string line = "row ";
      for (size_t i = 0; i < row.size(); ++i) line += (i ? "\t" : "") + (row[i] ? trimmed(*row[i]) : std::string("\\N"));
      out.push_back(line);
    }
  } catch (const sql::SyntaxError& e) {
    session.record_error(1064, e.what());
    out.push_back("error 1064|42000|" + std::string(e.what()));
  } catch (const sql::EmptyQuery& e) {
    session.record_error(1065, e.what());
    out.push_back("error 1065|42000|" + std::string(e.what()));
  } catch (const exec::SqlError& e) {
    out.push_back("error " + std::to_string(e.code()) + "|" + e.sqlstate() + "|" + e.what());
  }
  return out;
}

void run(const std::string& name) {
  static const exec::VariableRegistry registry("replicated", "8.4.13-kvasir", "Kvasir - The Distributed Relational Database (in memory, not durable)");
  catalog::Catalog node;
  txn::Store rows;
  exec::Session session(registry, node, rows, exec::SessionInfo{"root", "localhost", 1});
  const auto cases = load(name);
  ASSERT_FALSE(cases.empty());
  size_t matched = 0;
  for (const auto& c : cases) {
    const auto got = answer(session, c.statement);
    if (got == c.answer) {
      ++matched;
      continue;
    }
    std::ostringstream diff;
    diff << "statement: " << c.statement << "\n  MySQL:\n";
    for (const auto& line : c.answer) diff << "    " << line << "\n";
    diff << "  Kvasir:\n";
    for (const auto& line : got) diff << "    " << line << "\n";
    ADD_FAILURE() << diff.str();
  }
  std::cout << "[ differential ] " << name << ": " << matched << " of " << cases.size() << " statements answer as MySQL 8.4 does\n";
}

TEST(Differential, SessionStatementsAnswerAsMySql) { run("session.expected"); }

}  // namespace
}  // namespace kvasir
