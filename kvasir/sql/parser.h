//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "kvasir/sql/ast.h"
#include "kvasir/sql/lexer.h"

// The SQL parser: a statement's text into its syntax tree, by recursive
// descent written by hand, with MySQL 8.4's operator precedence (the manual,
// "Operator Precedence"). A parser of untrusted input: its nesting is bounded,
// and anything it does not understand is MySQL's syntax error at the token
// where understanding stopped.
//
// The statements so far are those a client sends as it connects and the
// first a session needs: SELECT (a select list, FROM DUAL or simple tables,
// WHERE, ORDER BY, LIMIT), SET (variables, NAMES, CHARACTER SET), USE,
// BEGIN, START TRANSACTION, COMMIT, ROLLBACK. Others are refused as syntax
// errors until they are built.
namespace kvasir::sql {

inline constexpr uint16_t er_empty_query = 1065;

// ER_EMPTY_QUERY (1065, SQLSTATE 42000): "Query was empty", for text with
// nothing but space and comments.
class EmptyQuery : public std::runtime_error {
 public:
  EmptyQuery() : std::runtime_error("Query was empty") {}
};

struct ParserOptions {
  LexerOptions lexer;
  // sql_mode PIPES_AS_CONCAT: || is CONCAT, not OR.
  bool pipes_as_concat = false;
  // How deeply expressions may nest before the text is refused.
  size_t max_depth = 200;
  // A prepared statement's text: each ? is a parameter, numbered from 0 in
  // the order written. Otherwise ? is a syntax error, as MySQL's.
  bool placeholders = false;
};

// Every statement in `text`, split at semicolons. Throws SyntaxError, or
// EmptyQuery for text with no statement.
std::vector<Statement> parse(std::string_view text, const ParserOptions& options = {});

}  // namespace kvasir::sql
