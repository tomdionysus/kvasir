//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/sql/parser.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "kvasir/sql/keywords.h"

namespace kvasir::sql {

namespace {

// Reserved words that are also functions, called with parentheses.
constexpr std::array<std::string_view, 27> reserved_functions = {
    "CHAR",         "CONVERT", "CUME_DIST",  "DATABASE", "DENSE_RANK", "FIRST_VALUE", "GROUPING", "IF",        "INSERT",
    "INTERVAL",     "LAG",     "LAST_VALUE", "LEAD",     "LEFT",       "MATCH",       "MOD",      "NTH_VALUE", "NTILE",
    "PERCENT_RANK", "RANK",    "REPEAT",     "REPLACE",  "RIGHT",      "ROW_NUMBER",  "SCHEMA",   "VALUES",    "ROW",
};

// Reserved words that are functions with or without parentheses.
constexpr std::array<std::string_view, 9> niladic_functions = {
    "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP", "CURRENT_USER", "LOCALTIME", "LOCALTIMESTAMP", "UTC_DATE", "UTC_TIME", "UTC_TIMESTAMP",
};

constexpr std::array<std::string_view, 5> set_scopes = {"GLOBAL", "SESSION", "LOCAL", "PERSIST", "PERSIST_ONLY"};
constexpr std::array<std::string_view, 3> variable_scopes = {"GLOBAL", "SESSION", "LOCAL"};
// Words SET takes as a value without their being an expression.
constexpr std::array<std::string_view, 5> set_keywords = {"ON", "ALL", "BINARY", "ROW", "SYSTEM"};

template <size_t N>
bool one_of(std::string_view word, const std::array<std::string_view, N>& words) noexcept {
  for (const auto w : words)
    if (same_word(word, w)) return true;
  return false;
}

ExprPtr node(ExprKind kind, size_t offset, std::string value = {}) {
  auto e = std::make_unique<Expr>();
  e->kind = kind;
  e->offset = offset;
  e->value = std::move(value);
  return e;
}

ExprPtr binary(std::string op, ExprPtr left, ExprPtr right) {
  auto e = node(ExprKind::binary, left->offset, std::move(op));
  e->args.push_back(std::move(left));
  e->args.push_back(std::move(right));
  return e;
}

ExprPtr unary(std::string op, size_t offset, ExprPtr operand) {
  auto e = node(ExprKind::unary, offset, std::move(op));
  e->args.push_back(std::move(operand));
  return e;
}

class Parser {
 public:
  Parser(std::string_view text, const ParserOptions& options) : _text(text), _options(options), _lexer(text, options.lexer) {}

  std::vector<Statement> statements() {
    if (peek().kind == TokenKind::end) throw EmptyQuery();
    std::vector<Statement> out;
    for (;;) {
      const size_t start = peek().offset;
      auto statement = parse_statement();
      statement.offset = start;
      statement.length = _last_end - start;
      out.push_back(std::move(statement));
      if (symbol(";")) {
        take();
        if (peek().kind == TokenKind::end) break;
        continue;
      }
      if (peek().kind == TokenKind::end) break;
      fail(peek());
    }
    return out;
  }

 private:
  // Counts nesting while it lives.
  class Depth {
   public:
    explicit Depth(Parser& parser) : _parser(parser) {
      if (++_parser._depth > _parser._options.max_depth) _parser.fail(_parser.peek());
    }
    ~Depth() { --_parser._depth; }
    Depth(const Depth&) = delete;
    Depth& operator=(const Depth&) = delete;

   private:
    Parser& _parser;
  };

  const Token& peek(size_t n = 0) {
    while (_ahead.size() <= n) _ahead.push_back(_lexer.next());
    return _ahead[n];
  }
  Token take() {
    peek();
    Token token = std::move(_ahead.front());
    _ahead.pop_front();
    if (token.kind != TokenKind::end) _last_end = token.offset + token.length;
    return token;
  }
  [[noreturn]] void fail(const Token& token) const { throw SyntaxError(_text, token.offset, token.line); }

  bool symbol(std::string_view s, size_t n = 0) { return peek(n).kind == TokenKind::symbol && peek(n).value == s; }
  bool word(std::string_view w, size_t n = 0) { return peek(n).kind == TokenKind::word && same_word(peek(n).value, w); }
  bool name_token(size_t n = 0) { return peek(n).kind == TokenKind::quoted_identifier || (peek(n).kind == TokenKind::word && !is_reserved(peek(n).value)); }
  bool statement_ends(size_t n = 0) { return peek(n).kind == TokenKind::end || symbol(";", n); }

  void expect_symbol(std::string_view s) {
    if (!symbol(s)) fail(peek());
    take();
  }
  void expect_word(std::string_view w) {
    if (!word(w)) fail(peek());
    take();
  }
  bool accept_word(std::string_view w) {
    if (!word(w)) return false;
    take();
    return true;
  }

  // An unreserved word or a quoted identifier.
  std::string name() {
    if (!name_token()) fail(peek());
    return take().value;
  }
  // After a qualifier's dot any word is a name.
  std::string name_after_dot() {
    if (peek().kind != TokenKind::word && peek().kind != TokenKind::quoted_identifier) fail(peek());
    return take().value;
  }
  // A character set or collation: a name or a string, or BINARY, a reserved
  // word, for the binary character set.
  std::string charset_name() {
    if (peek().kind == TokenKind::string || name_token()) return take().value;
    if (word("BINARY")) {
      take();
      return "binary";
    }
    fail(peek());
  }

  Statement parse_statement() {
    Statement s;
    if (word("SELECT")) {
      s.kind = Statement::Kind::select;
      s.select = parse_select();
    } else if (word("SET")) {
      s.kind = Statement::Kind::set;
      take();
      do s.assignments.push_back(parse_assignment());
      while (symbol(",") && (take(), true));
    } else if (word("USE")) {
      s.kind = Statement::Kind::use;
      take();
      s.database = name();
    } else if (word("BEGIN")) {
      s.kind = Statement::Kind::begin;
      take();
      accept_word("WORK");
    } else if (word("START")) {
      s.kind = Statement::Kind::begin;
      take();
      expect_word("TRANSACTION");
      parse_characteristics(s);
    } else if (word("COMMIT")) {
      s.kind = Statement::Kind::commit;
      take();
      if (peek().kind == TokenKind::hint) s.hint = take().value;
      accept_word("WORK");
    } else if (word("INSERT")) {
      take();
      s.kind = Statement::Kind::insert;
      parse_insert(s);
    } else if (word("REPLACE")) {
      s.kind = Statement::Kind::insert;
      unsupported(s, "REPLACE");
    } else if (word("CREATE") && (word("TABLE", 1) || word("TEMPORARY", 1))) {
      take();
      s.kind = Statement::Kind::create_table;
      if (accept_word("TEMPORARY")) {
        unsupported(s, "CREATE TEMPORARY TABLE");
      } else {
        take();  // TABLE
        parse_create_table(s);
      }
    } else if (word("CREATE")) {
      take();
      if (!accept_word("DATABASE")) expect_word("SCHEMA");
      s.kind = Statement::Kind::create_database;
      if (accept_word("IF")) {
        expect_word("NOT");
        expect_word("EXISTS");
        s.if_exists = true;
      }
      s.database = name();
      parse_database_options(s);
    } else if (word("DROP") && (word("TABLE", 1) || word("TABLES", 1) || word("TEMPORARY", 1))) {
      take();
      s.kind = Statement::Kind::drop_table;
      if (accept_word("TEMPORARY")) {
        unsupported(s, "DROP TEMPORARY TABLE");
      } else {
        take();  // TABLE or TABLES
        if (accept_word("IF")) {
          expect_word("EXISTS");
          s.if_exists = true;
        }
        do s.tables.push_back(table_name());
        while (symbol(",") && (take(), true));
        if (!accept_word("RESTRICT")) accept_word("CASCADE");
      }
    } else if (word("DROP")) {
      take();
      if (!accept_word("DATABASE")) expect_word("SCHEMA");
      s.kind = Statement::Kind::drop_database;
      if (accept_word("IF")) {
        expect_word("EXISTS");
        s.if_exists = true;
      }
      s.database = name();
    } else if (word("SHOW") && (word("TABLES", 1) || word("FULL", 1))) {
      take();
      s.kind = Statement::Kind::show_tables;
      s.full = accept_word("FULL");
      expect_word("TABLES");
      if (accept_word("FROM") || accept_word("IN")) s.database = name();
      if (accept_word("LIKE")) {
        if (peek().kind != TokenKind::string) fail(peek());
        s.like = take().value;
      } else if (word("WHERE")) {
        unsupported(s, "SHOW TABLES ... WHERE");
      }
    } else if (word("SHOW") && (word("DATABASES", 1) || word("SCHEMAS", 1))) {
      take();
      take();
      s.kind = Statement::Kind::show_databases;
      if (accept_word("LIKE")) {
        if (peek().kind != TokenKind::string) fail(peek());
        s.like = take().value;
      }
    } else if (word("SHOW")) {
      s.kind = Statement::Kind::show_warnings;
      take();
      if (accept_word("COUNT")) {
        expect_symbol("(");
        expect_symbol("*");
        expect_symbol(")");
        s.count_only = true;
      }
      if (accept_word("ERRORS")) {
        s.errors_only = true;
      } else {
        expect_word("WARNINGS");
      }
      if (!s.count_only && accept_word("LIMIT")) {
        auto first = limit_value();
        if (symbol(",")) {
          take();
          s.select.offset = std::move(first);
          s.select.limit = limit_value();
        } else {
          s.select.limit = std::move(first);
        }
      }
    } else if (word("ROLLBACK")) {
      s.kind = Statement::Kind::rollback;
      take();
      accept_word("WORK");
    } else {
      fail(peek());
    }
    return s;
  }

  // CREATE DATABASE's [DEFAULT] CHARACTER SET | CHARSET [=] name and
  // [DEFAULT] COLLATE [=] name, in any order.
  void parse_database_options(Statement& s) {
    while (!statement_ends()) {
      accept_word("DEFAULT");
      std::string* target = nullptr;
      if (accept_word("CHARACTER")) {
        expect_word("SET");
        target = &s.charset;
      } else if (accept_word("CHARSET")) {
        target = &s.charset;
      } else if (accept_word("COLLATE")) {
        target = &s.collation;
      } else {
        fail(peek());
      }
      if (symbol("=")) take();
      *target = charset_name();
    }
  }

  // Marks the statement as valid MySQL Kvasir does not yet carry out, and
  // passes over the rest of it.
  void unsupported(Statement& s, std::string what) {
    s.unsupported = std::move(what);
    int depth = 0;
    while (peek().kind != TokenKind::end && !(depth <= 0 && symbol(";"))) {
      if (symbol("(")) ++depth;
      if (symbol(")")) --depth;
      take();
    }
  }

  // [schema.]table
  std::vector<std::string> table_name() {
    std::vector<std::string> out{name()};
    if (symbol(".")) {
      take();
      out.push_back(name_after_dot());
    }
    return out;
  }

  uint64_t number() {
    if (peek().kind != TokenKind::integer) fail(peek());
    const auto text = take().value;
    uint64_t v = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
    return r.ec == std::errc() ? v : UINT64_MAX;
  }

  std::string quoted_string() {
    if (peek().kind != TokenKind::string) fail(peek());
    return take().value;
  }

  void accept_equals() {
    if (symbol("=")) take();
  }

  // INSERT after INSERT: [LOW_PRIORITY | HIGH_PRIORITY] [INTO] table, then
  // [(columns)] VALUES | VALUE (row), ..., or SET column = value, ....
  void parse_insert(Statement& s) {
    auto& in = s.insert;
    if (word("DELAYED")) return unsupported(s, "INSERT DELAYED");
    if (!accept_word("LOW_PRIORITY")) accept_word("HIGH_PRIORITY");
    if (word("IGNORE")) return unsupported(s, "INSERT IGNORE");
    accept_word("INTO");
    in.table = table_name();
    if (word("PARTITION")) return unsupported(s, "INSERT ... PARTITION");
    if (accept_word("SET")) {
      in.has_columns = true;
      in.rows.emplace_back();
      do {
        in.columns.push_back(name());
        if (!symbol("=") && !symbol(":=")) fail(peek());
        take();
        in.rows.back().push_back(insert_value());
      } while (symbol(",") && (take(), true));
    } else {
      if (symbol("(") && !word("SELECT", 1)) {
        take();
        in.has_columns = true;
        if (!symbol(")")) {
          do in.columns.push_back(name());
          while (symbol(",") && (take(), true));
        }
        expect_symbol(")");
      }
      if (word("SELECT") || (symbol("(") && word("SELECT", 1))) return unsupported(s, "INSERT ... SELECT");
      if (word("TABLE")) return unsupported(s, "INSERT ... TABLE");
      if (!accept_word("VALUES")) expect_word("VALUE");
      do {
        expect_symbol("(");
        in.rows.emplace_back();
        if (!symbol(")")) {
          do in.rows.back().push_back(insert_value());
          while (symbol(",") && (take(), true));
        }
        expect_symbol(")");
      } while (symbol(",") && (take(), true));
    }
    if (word("AS")) return unsupported(s, "INSERT ... AS");
    if (word("ON")) return unsupported(s, "ON DUPLICATE KEY UPDATE");
  }

  // A value in INSERT: an expression, or DEFAULT.
  ExprPtr insert_value() {
    if (word("DEFAULT") && (symbol(",", 1) || symbol(")", 1) || statement_ends(1))) return node(ExprKind::default_value, take().offset, "DEFAULT");
    return expression();
  }

  // CREATE TABLE after TABLE: the manual's create_definition list and
  // table_options, as far as Kvasir carries them out.
  void parse_create_table(Statement& s) {
    auto& t = s.create_table;
    if (accept_word("IF")) {
      expect_word("NOT");
      expect_word("EXISTS");
      t.if_not_exists = true;
    }
    t.name = table_name();
    if (word("LIKE") || (symbol("(") && word("LIKE", 1))) return unsupported(s, "CREATE TABLE ... LIKE");
    if (word("AS") || word("SELECT") || word("IGNORE") || word("REPLACE") || (symbol("(") && word("SELECT", 1)))
      return unsupported(s, "CREATE TABLE ... SELECT");
    expect_symbol("(");
    do {
      table_element(s);
      if (!s.unsupported.empty()) return;
    } while (symbol(",") && (take(), true));
    expect_symbol(")");
    table_options(s);
  }

  void table_element(Statement& s) {
    auto& t = s.create_table;
    std::string constraint;
    bool named_constraint = false;
    if (accept_word("CONSTRAINT")) {
      named_constraint = true;
      if (name_token()) constraint = take().value;
      if (!word("PRIMARY") && !word("UNIQUE") && !word("FOREIGN") && !word("CHECK")) fail(peek());
    }
    if (word("CHECK")) return unsupported(s, "CHECK");
    if (word("FOREIGN")) return unsupported(s, "FOREIGN KEY");
    if (word("FULLTEXT")) return unsupported(s, "FULLTEXT");
    if (word("SPATIAL")) return unsupported(s, "SPATIAL");
    KeySpec key;
    key.position = t.columns.size();
    if (accept_word("PRIMARY")) {
      expect_word("KEY");
      key.kind = KeySpec::Kind::primary;
    } else if (accept_word("UNIQUE")) {
      if (!accept_word("INDEX")) accept_word("KEY");
      key.kind = KeySpec::Kind::unique;
      key.name = constraint;
    } else if (!named_constraint && (accept_word("INDEX") || accept_word("KEY"))) {
      key.kind = KeySpec::Kind::index;
    } else {
      return column_definition(s);
    }
    if (name_token()) {
      const auto n = take().value;
      if (key.kind != KeySpec::Kind::primary) key.name = n;
    }
    index_type();
    expect_symbol("(");
    do {
      if (symbol("(")) return unsupported(s, "functional key parts");
      KeyPart part;
      part.column = name();
      if (symbol("(")) {
        take();
        part.prefix = number();
        expect_symbol(")");
      }
      if (accept_word("DESC")) {
        part.descending = true;
      } else {
        accept_word("ASC");
      }
      key.parts.push_back(std::move(part));
    } while (symbol(",") && (take(), true));
    expect_symbol(")");
    while (!symbol(",") && !symbol(")")) {
      if (accept_word("COMMENT")) {
        key.comment = quoted_string();
      } else if (word("USING") || word("TYPE")) {
        index_type();
      } else if (word("KEY_BLOCK_SIZE") || word("VISIBLE") || word("INVISIBLE") || word("WITH") || word("ENGINE_ATTRIBUTE") ||
                 word("SECONDARY_ENGINE_ATTRIBUTE")) {
        return unsupported(s, "index option " + upper(peek().value));
      } else {
        fail(peek());
      }
    }
    t.keys.push_back(std::move(key));
  }

  // USING BTREE or HASH: InnoDB's indexes are B-trees whichever is named.
  void index_type() {
    if (!accept_word("USING") && !accept_word("TYPE")) return;
    if (!accept_word("BTREE")) expect_word("HASH");
  }

  void column_definition(Statement& s) {
    ColumnSpec c;
    c.name = name();
    data_type(s, c);
    if (!s.unsupported.empty()) return;
    while (!symbol(",") && !symbol(")")) {
      if (accept_word("NOT")) {
        expect_word("NULL");
        c.nullable = false;
      } else if (accept_word("NULL")) {
        c.nullable = true;
      } else if (accept_word("DEFAULT")) {
        if (symbol("(")) return unsupported(s, "DEFAULT (expression)");
        c.default_value = default_value();
      } else if (accept_word("AUTO_INCREMENT")) {
        c.auto_increment = true;
      } else if (accept_word("UNIQUE")) {
        accept_word("KEY");
        c.unique = true;
      } else if (accept_word("PRIMARY")) {
        expect_word("KEY");
        c.primary = true;
      } else if (accept_word("KEY")) {
        c.primary = true;
      } else if (accept_word("COMMENT")) {
        c.comment = quoted_string();
      } else if (accept_word("COLLATE")) {
        c.type.collation = charset_name();
      } else if (accept_word("SERIAL")) {
        expect_word("DEFAULT");
        expect_word("VALUE");
        c.nullable = false;
        c.auto_increment = true;
        c.unique = true;
      } else if (word("CHECK") || word("CONSTRAINT")) {
        return unsupported(s, "CHECK");
      } else if (word("REFERENCES")) {
        return unsupported(s, "REFERENCES");
      } else if (word("AS") || word("GENERATED")) {
        return unsupported(s, "generated columns");
      } else if (word("ON")) {
        return unsupported(s, "ON UPDATE");
      } else if (word("VISIBLE") || word("INVISIBLE") || word("COLUMN_FORMAT") || word("STORAGE") || word("ENGINE_ATTRIBUTE") ||
                 word("SECONDARY_ENGINE_ATTRIBUTE") || word("SRID")) {
        return unsupported(s, "column attribute " + upper(peek().value));
      } else {
        fail(peek());
      }
    }
    s.create_table.columns.push_back(std::move(c));
  }

  // DEFAULT's value: a literal, signed or not, NULL, TRUE or FALSE, or one of
  // the time functions (which only a temporal column takes).
  ExprPtr default_value() {
    std::vector<Token> signs;
    while (symbol("-") || symbol("+")) signs.push_back(take());
    const auto kind = peek().kind;
    const bool literal = kind == TokenKind::integer || kind == TokenKind::decimal || kind == TokenKind::floating || kind == TokenKind::string ||
                         kind == TokenKind::national_string || kind == TokenKind::hex || kind == TokenKind::bits ||
                         (kind == TokenKind::word && (word("NULL") || word("TRUE") || word("FALSE") || peek().value.starts_with("_")));
    ExprPtr e;
    if (literal) {
      e = primary();
    } else if (signs.empty() && kind == TokenKind::word && (one_of(peek().value, niladic_functions) || word("NOW"))) {
      e = node(ExprKind::call, peek().offset, upper(take().value));
      if (symbol("(")) {
        take();
        if (!symbol(")")) e->args.push_back(primary());
        expect_symbol(")");
      }
      return e;
    } else {
      fail(peek());
    }
    for (auto it = signs.rbegin(); it != signs.rend(); ++it)
      if (it->value == "-") e = unary("-", it->offset, std::move(e));
    return e;
  }

  std::optional<uint64_t> length() {
    if (!symbol("(")) return std::nullopt;
    take();
    const auto n = number();
    expect_symbol(")");
    return n;
  }

  // (M), (M,D) or nothing.
  void precision(DataType& t, bool scale_required = false) {
    if (!symbol("(")) return;
    take();
    t.length = number();
    if (scale_required || symbol(",")) {
      expect_symbol(",");
      t.scale = number();
    }
    expect_symbol(")");
  }

  void numeric_attributes(DataType& t) {
    for (;;) {
      if (accept_word("UNSIGNED")) {
        t.is_unsigned = true;
      } else if (accept_word("SIGNED")) {
      } else if (accept_word("ZEROFILL")) {
        t.zerofill = true;
      } else {
        return;
      }
    }
  }

  void string_attributes(DataType& t) {
    for (;;) {
      if (accept_word("CHARACTER")) {
        expect_word("SET");
        t.charset = charset_name();
      } else if (accept_word("CHARSET")) {
        t.charset = charset_name();
      } else if (accept_word("BINARY")) {
        t.binary_attribute = true;
      } else if (accept_word("ASCII")) {
        t.charset = "latin1";
      } else if (accept_word("UNICODE")) {
        t.charset = "ucs2";
      } else {
        return;
      }
    }
  }

  // The manual's data types; the ones Kvasir does not yet hold are marked.
  void data_type(Statement& s, ColumnSpec& c) {
    if (peek().kind != TokenKind::word) fail(peek());
    auto& t = c.type;
    const std::string w = upper(peek().value);
    const auto integer = [&](const char* type_name) {
      take();
      t.name = type_name;
      t.length = length();
      numeric_attributes(t);
    };
    if (w == "TINYINT" || w == "INT1") return integer("TINYINT");
    if (w == "SMALLINT" || w == "INT2") return integer("SMALLINT");
    if (w == "MEDIUMINT" || w == "INT3" || w == "MIDDLEINT") return integer("MEDIUMINT");
    if (w == "INT" || w == "INTEGER" || w == "INT4") return integer("INT");
    if (w == "BIGINT" || w == "INT8") return integer("BIGINT");
    if (w == "BOOL" || w == "BOOLEAN") {
      take();
      t.name = "BOOL";
      return;
    }
    if (w == "SERIAL") {
      take();
      t.name = "BIGINT";
      t.is_unsigned = true;
      c.nullable = false;
      c.auto_increment = true;
      c.unique = true;
      return;
    }
    if (w == "DECIMAL" || w == "DEC" || w == "NUMERIC" || w == "FIXED") {
      take();
      t.name = "DECIMAL";
      precision(t);
      numeric_attributes(t);
      return;
    }
    if (w == "FLOAT" || w == "FLOAT4") {
      take();
      t.name = "FLOAT";
      precision(t);
      numeric_attributes(t);
      return;
    }
    if (w == "DOUBLE" || w == "FLOAT8" || w == "REAL") {
      take();
      if (w == "DOUBLE") accept_word("PRECISION");
      t.name = "DOUBLE";
      precision(t, true);
      numeric_attributes(t);
      return;
    }
    const auto text = [&](const char* type_name, bool length_required) {
      t.name = type_name;
      if (length_required && !symbol("(")) fail(peek());
      t.length = length();
      string_attributes(t);
    };
    if (w == "NATIONAL" || w == "NCHAR" || w == "NVARCHAR") {
      take();
      t.charset = "utf8mb3";
      if (w == "NVARCHAR") return text("VARCHAR", true);
      if (w == "NATIONAL") {
        if (accept_word("VARCHAR")) return text("VARCHAR", true);
        if (!accept_word("CHAR")) expect_word("CHARACTER");
      }
      if (accept_word("VARCHAR") || accept_word("VARYING")) return text("VARCHAR", true);
      return text("CHAR", false);
    }
    if (w == "CHAR" || w == "CHARACTER") {
      take();
      if (accept_word("VARYING")) return text("VARCHAR", true);
      return text("CHAR", false);
    }
    if (w == "VARCHAR" || w == "VARCHARACTER") {
      take();
      return text("VARCHAR", true);
    }
    if (w == "BINARY" || w == "VARBINARY") {
      take();
      t.name = w;
      if (w == "VARBINARY" && !symbol("(")) fail(peek());
      t.length = length();
      return;
    }
    if (w == "TEXT" || w == "BLOB") {
      take();
      t.name = w;
      t.length = length();
      if (w == "TEXT") string_attributes(t);
      return;
    }
    if (w == "TINYTEXT" || w == "MEDIUMTEXT" || w == "LONGTEXT") {
      take();
      t.name = w;
      string_attributes(t);
      return;
    }
    if (w == "TINYBLOB" || w == "MEDIUMBLOB" || w == "LONGBLOB") {
      take();
      t.name = w;
      return;
    }
    if (w == "LONG") {
      take();
      if (accept_word("VARBINARY")) {
        t.name = "MEDIUMBLOB";
        return;
      }
      accept_word("VARCHAR");
      t.name = "MEDIUMTEXT";
      string_attributes(t);
      return;
    }
    static constexpr std::array<std::string_view, 19> later = {"BIT",
                                                               "DATE",
                                                               "DATETIME",
                                                               "TIMESTAMP",
                                                               "TIME",
                                                               "YEAR",
                                                               "ENUM",
                                                               "SET",
                                                               "JSON",
                                                               "GEOMETRY",
                                                               "POINT",
                                                               "LINESTRING",
                                                               "POLYGON",
                                                               "MULTIPOINT",
                                                               "MULTILINESTRING",
                                                               "MULTIPOLYGON",
                                                               "GEOMETRYCOLLECTION",
                                                               "GEOMCOLLECTION",
                                                               "VECTOR"};
    if (one_of(w, later)) return unsupported(s, "the " + w + " type");
    fail(peek());
  }

  // CREATE TABLE's table_options, with or without commas between them.
  void table_options(Statement& s) {
    auto& t = s.create_table;
    static constexpr std::array<std::string_view, 25> later = {"ROW_FORMAT",
                                                               "AVG_ROW_LENGTH",
                                                               "CHECKSUM",
                                                               "COMPRESSION",
                                                               "CONNECTION",
                                                               "DATA",
                                                               "INDEX",
                                                               "DELAY_KEY_WRITE",
                                                               "ENCRYPTION",
                                                               "INSERT_METHOD",
                                                               "KEY_BLOCK_SIZE",
                                                               "MAX_ROWS",
                                                               "MIN_ROWS",
                                                               "PACK_KEYS",
                                                               "PASSWORD",
                                                               "STATS_AUTO_RECALC",
                                                               "STATS_PERSISTENT",
                                                               "STATS_SAMPLE_PAGES",
                                                               "TABLESPACE",
                                                               "UNION",
                                                               "ENGINE_ATTRIBUTE",
                                                               "SECONDARY_ENGINE",
                                                               "SECONDARY_ENGINE_ATTRIBUTE",
                                                               "AUTOEXTEND_SIZE",
                                                               "START"};
    while (!statement_ends()) {
      if (symbol(",")) take();
      if (accept_word("ENGINE") || accept_word("TYPE")) {
        accept_equals();
        if (peek().kind != TokenKind::string && !name_token()) fail(peek());
        t.engine = take().value;
        continue;
      }
      if (word("AS") || word("SELECT") || word("IGNORE") || word("REPLACE") || (symbol("(") && word("SELECT", 1)))
        return unsupported(s, "CREATE TABLE ... SELECT");
      if (word("PARTITION")) return unsupported(s, "PARTITION BY");
      if (accept_word("AUTO_INCREMENT")) {
        accept_equals();
        t.auto_increment = number();
        continue;
      }
      if (accept_word("COMMENT")) {
        accept_equals();
        t.comment = quoted_string();
        continue;
      }
      const bool given_default = accept_word("DEFAULT");
      if (accept_word("CHARACTER")) {
        expect_word("SET");
        accept_equals();
        t.charset = charset_name();
      } else if (accept_word("CHARSET")) {
        accept_equals();
        t.charset = charset_name();
      } else if (accept_word("COLLATE")) {
        accept_equals();
        t.collation = charset_name();
      } else if (!given_default && peek().kind == TokenKind::word && one_of(peek().value, later)) {
        return unsupported(s, "table option " + upper(peek().value));
      } else {
        fail(peek());
      }
    }
  }

  void parse_characteristics(Statement& s) {
    if (statement_ends()) return;
    do {
      if (accept_word("WITH")) {
        expect_word("CONSISTENT");
        expect_word("SNAPSHOT");
      } else if (accept_word("READ")) {
        if (accept_word("ONLY")) {
          s.read_only = true;
        } else {
          expect_word("WRITE");
          s.read_only = false;
        }
      } else {
        fail(peek());
      }
    } while (symbol(",") && (take(), true));
  }

  Select parse_select() {
    Select select;
    take();  // SELECT
    if (peek().kind == TokenKind::hint) select.hint = take().value;
    if (accept_word("DISTINCT") || accept_word("DISTINCTROW")) {
      select.distinct = true;
    } else {
      accept_word("ALL");
    }
    do select.items.push_back(parse_item());
    while (symbol(",") && (take(), true));

    if (accept_word("FROM")) {
      if (accept_word("DUAL")) {
        select.from_dual = true;
      } else {
        do select.from.push_back(parse_table());
        while (symbol(",") && (take(), true));
      }
    }
    if (accept_word("WHERE")) select.where = expression();
    if (accept_word("ORDER")) {
      expect_word("BY");
      do {
        OrderItem item;
        item.expr = expression();
        if (accept_word("DESC")) {
          item.descending = true;
        } else {
          accept_word("ASC");
        }
        select.order.push_back(std::move(item));
      } while (symbol(",") && (take(), true));
    }
    if (accept_word("LIMIT")) {
      auto first = limit_value();
      if (symbol(",")) {
        take();
        select.offset = std::move(first);
        select.limit = limit_value();
      } else {
        select.limit = std::move(first);
        if (accept_word("OFFSET")) select.offset = limit_value();
      }
    }
    return select;
  }

  ExprPtr limit_value() {
    const auto kind = peek().kind;
    const auto offset = peek().offset;
    if (kind == TokenKind::integer) return node(ExprKind::integer, offset, take().value);
    if (kind == TokenKind::placeholder) return node(ExprKind::placeholder, take().offset);
    fail(peek());
  }

  SelectItem parse_item() {
    SelectItem item;
    const size_t start = peek().offset;
    if (symbol("*")) {
      take();
      item.star = true;
      item.text = "*";
      return item;
    }
    // name.*, name.name.*
    for (size_t i = 0; (peek(i).kind == TokenKind::word || peek(i).kind == TokenKind::quoted_identifier) && symbol(".", i + 1); i += 2) {
      if (!symbol("*", i + 2)) continue;
      if (!name_token(0)) fail(peek());
      for (size_t k = 0; k <= i; k += 2) {
        item.star_path.push_back(take().value);
        take();  // .
      }
      take();  // *
      item.star = true;
      item.text = std::string(_text.substr(start, _last_end - start));
      return item;
    }
    item.expr = expression();
    item.text = std::string(_text.substr(start, _last_end - start));
    if (accept_word("AS")) {
      if (peek().kind != TokenKind::string && !name_token()) fail(peek());
      item.has_alias = true;
      item.alias = take().value;
    } else if (peek().kind == TokenKind::string || name_token()) {
      item.has_alias = true;
      item.alias = take().value;
    }
    return item;
  }

  TableRef parse_table() {
    TableRef table;
    table.name.push_back(name());
    if (symbol(".")) {
      take();
      table.name.push_back(name_after_dot());
    }
    if (accept_word("AS")) {
      table.alias = name();
    } else if (name_token()) {
      table.alias = take().value;
    }
    return table;
  }

  Assignment parse_assignment() {
    Assignment a;
    const bool assigns = symbol("=", 1) || symbol(":=", 1);
    if (word("NAMES") && !assigns) {
      take();
      a.target = Assignment::Target::names;
      if (accept_word("DEFAULT")) {
        a.use_default = true;
      } else {
        a.charset = charset_name();
        if (accept_word("COLLATE")) a.collation = charset_name();
      }
      return a;
    }
    if ((word("CHARACTER") && word("SET", 1)) || (word("CHARSET") && !assigns)) {
      if (take().value.size() == 9) take();  // CHARACTER SET
      a.target = Assignment::Target::character_set;
      if (accept_word("DEFAULT")) {
        a.use_default = true;
      } else {
        a.charset = charset_name();
      }
      return a;
    }
    if (peek().kind == TokenKind::user_variable) {
      a.target = Assignment::Target::user_variable;
      a.name = take().value;
    } else {
      a.target = Assignment::Target::system_variable;
      if (peek().kind == TokenKind::word && one_of(peek().value, set_scopes) &&
          (peek(1).kind == TokenKind::word || peek(1).kind == TokenKind::quoted_identifier)) {
        a.scope = upper(take().value);
        a.name = name();
      } else if (symbol("@@")) {
        take();
        if (peek().kind == TokenKind::word && one_of(peek().value, variable_scopes) && symbol(".", 1)) {
          a.scope = upper(take().value);
          take();
        }
        a.name = name_after_dot();
      } else {
        a.name = name();
      }
    }
    if (!symbol("=") && !symbol(":=")) fail(peek());
    take();
    a.value = set_value();
    return a;
  }

  ExprPtr set_value() {
    const auto& t = peek();
    if (word("DEFAULT")) return node(ExprKind::default_value, take().offset, "DEFAULT");
    if (t.kind == TokenKind::word && one_of(t.value, set_keywords) && (symbol(",", 1) || statement_ends(1))) {
      const auto offset = t.offset;
      return node(ExprKind::keyword_value, offset, upper(take().value));
    }
    return expression();
  }

  // Expressions, lowest precedence first.

  ExprPtr expression() {
    Depth depth(*this);
    return assignment();
  }

  ExprPtr assignment() {
    auto left = or_expr();
    if (symbol(":=") && left->kind == ExprKind::user_variable) {
      take();
      Depth depth(*this);
      auto e = node(ExprKind::assign, left->offset);
      e->args.push_back(std::move(left));
      e->args.push_back(assignment());
      return e;
    }
    return left;
  }

  ExprPtr or_expr() {
    auto left = xor_expr();
    for (;;) {
      if (word("OR")) {
        take();
        left = binary("OR", std::move(left), xor_expr());
      } else if (symbol("||")) {
        take();
        if (_options.pipes_as_concat) {
          auto e = node(ExprKind::call, left->offset, "CONCAT");
          e->args.push_back(std::move(left));
          e->args.push_back(xor_expr());
          left = std::move(e);
        } else {
          left = binary("OR", std::move(left), xor_expr());
        }
      } else {
        return left;
      }
    }
  }

  ExprPtr xor_expr() {
    auto left = and_expr();
    while (word("XOR")) {
      take();
      left = binary("XOR", std::move(left), and_expr());
    }
    return left;
  }

  ExprPtr and_expr() {
    auto left = not_expr();
    while (word("AND") || symbol("&&")) {
      take();
      left = binary("AND", std::move(left), not_expr());
    }
    return left;
  }

  ExprPtr not_expr() {
    if (word("NOT")) {
      const auto offset = take().offset;
      Depth depth(*this);
      return unary("NOT", offset, not_expr());
    }
    return bool_primary();
  }

  ExprPtr bool_primary() {
    auto left = predicate();
    for (;;) {
      std::string op;
      if (peek().kind == TokenKind::symbol) {
        const auto& v = peek().value;
        if (v == "=" || v == "<=>" || v == "<" || v == "<=" || v == ">" || v == ">=" || v == "!=") op = v;
        if (v == "<>") op = "!=";
      }
      if (!op.empty()) {
        take();
        left = binary(op, std::move(left), predicate());
        continue;
      }
      if (word("IS")) {
        take();
        auto e = node(ExprKind::is, left->offset);
        e->negated = accept_word("NOT");
        if (!word("NULL") && !word("TRUE") && !word("FALSE") && !word("UNKNOWN")) fail(peek());
        e->value = upper(take().value);
        e->args.push_back(std::move(left));
        left = std::move(e);
        continue;
      }
      return left;
    }
  }

  ExprPtr predicate() {
    auto left = bit_or();
    bool negated = false;
    if (word("NOT") && (word("IN", 1) || word("BETWEEN", 1) || word("LIKE", 1) || word("REGEXP", 1) || word("RLIKE", 1))) {
      take();
      negated = true;
    }
    if (word("IN")) {
      take();
      auto e = node(ExprKind::in, left->offset);
      e->negated = negated;
      e->args.push_back(std::move(left));
      expect_symbol("(");
      do e->args.push_back(expression());
      while (symbol(",") && (take(), true));
      expect_symbol(")");
      return e;
    }
    if (word("BETWEEN")) {
      take();
      auto e = node(ExprKind::between, left->offset);
      e->negated = negated;
      e->args.push_back(std::move(left));
      e->args.push_back(bit_or());
      expect_word("AND");
      Depth depth(*this);
      e->args.push_back(predicate());
      return e;
    }
    if (word("LIKE")) {
      take();
      auto e = node(ExprKind::like, left->offset);
      e->negated = negated;
      e->args.push_back(std::move(left));
      e->args.push_back(unary_expr());
      if (accept_word("ESCAPE")) e->args.push_back(unary_expr());
      return e;
    }
    if (word("REGEXP") || word("RLIKE")) {
      take();
      auto e = node(ExprKind::regexp, left->offset);
      e->negated = negated;
      e->args.push_back(std::move(left));
      e->args.push_back(bit_or());
      return e;
    }
    if (negated) fail(peek());
    return left;
  }

  ExprPtr bit_or() {
    auto left = bit_and();
    while (symbol("|")) {
      take();
      left = binary("|", std::move(left), bit_and());
    }
    return left;
  }

  ExprPtr bit_and() {
    auto left = shift();
    while (symbol("&")) {
      take();
      left = binary("&", std::move(left), shift());
    }
    return left;
  }

  ExprPtr shift() {
    auto left = additive();
    while (symbol("<<") || symbol(">>")) {
      auto op = take().value;
      left = binary(op, std::move(left), additive());
    }
    return left;
  }

  ExprPtr additive() {
    auto left = multiplicative();
    while (symbol("+") || symbol("-")) {
      auto op = take().value;
      left = binary(op, std::move(left), multiplicative());
    }
    return left;
  }

  ExprPtr multiplicative() {
    auto left = bit_xor();
    for (;;) {
      std::string op;
      if (symbol("*") || symbol("/") || symbol("%")) op = peek().value;
      if (word("DIV")) op = "DIV";
      if (word("MOD")) op = "%";
      if (op.empty()) return left;
      take();
      left = binary(op, std::move(left), bit_xor());
    }
  }

  ExprPtr bit_xor() {
    auto left = unary_expr();
    while (symbol("^")) {
      take();
      left = binary("^", std::move(left), unary_expr());
    }
    return left;
  }

  ExprPtr unary_expr() {
    Depth depth(*this);
    if (symbol("-") || symbol("~")) {
      const auto t = take();
      return unary(t.value, t.offset, unary_expr());
    }
    if (symbol("+")) {
      take();
      return unary_expr();
    }
    if (symbol("!")) {
      const auto offset = take().offset;
      return unary("NOT", offset, unary_expr());
    }
    if (word("BINARY") && !symbol("(", 1)) {
      const auto offset = take().offset;
      return unary("BINARY", offset, unary_expr());
    }
    auto e = primary();
    while (word("COLLATE")) {
      take();
      // A collation is named, never BINARY (observed: a syntax error).
      if (word("BINARY")) fail(peek());
      auto c = node(ExprKind::collate, e->offset, charset_name());
      c->args.push_back(std::move(e));
      e = std::move(c);
    }
    return e;
  }

  // A string and the strings after it, joined.
  ExprPtr strings(size_t offset, std::string charset) {
    auto e = node(ExprKind::string, offset);
    e->charset = std::move(charset);
    while (peek().kind == TokenKind::string || peek().kind == TokenKind::national_string) {
      if (peek().kind == TokenKind::national_string) {
        if (!e->value.empty() || e->charset.size()) fail(peek());
        e->charset = "national";
      }
      e->path.push_back(take().value);
      e->value += e->path.back();
    }
    return e;
  }

  ExprPtr primary() {
    // Copied, not referred to: take() frees the token peek() returned, and a
    // call's arguments may be evaluated in any order.
    const auto kind = peek().kind;
    const auto offset = peek().offset;
    switch (kind) {
      case TokenKind::integer:
        return node(ExprKind::integer, offset, take().value);
      case TokenKind::decimal:
        return node(ExprKind::decimal, offset, take().value);
      case TokenKind::floating:
        return node(ExprKind::floating, offset, take().value);
      case TokenKind::hex:
        return node(ExprKind::hex, offset, take().value);
      case TokenKind::bits:
        return node(ExprKind::bits, offset, take().value);
      case TokenKind::placeholder:
        return node(ExprKind::placeholder, take().offset);
      case TokenKind::string:
      case TokenKind::national_string:
        return strings(offset, "");
      case TokenKind::user_variable:
        return node(ExprKind::user_variable, offset, take().value);
      case TokenKind::quoted_identifier:
        return column();
      case TokenKind::word:
        return word_primary();
      case TokenKind::symbol:
        if (symbol("@@")) return system_variable();
        if (symbol("(")) return parenthesised();
        break;
      default:
        break;
    }
    fail(peek());
  }

  ExprPtr word_primary() {
    const Token& t = peek();
    const auto offset = t.offset;
    if (word("NULL")) return node(ExprKind::null, take().offset, "NULL");
    if (word("TRUE") || word("FALSE")) return node(ExprKind::boolean, offset, upper(take().value));
    if (t.value.size() > 1 && t.value[0] == '_' && peek(1).kind == TokenKind::string) {
      auto charset = take().value.substr(1);
      return strings(offset, std::move(charset));
    }
    if (word("CASE")) return case_when();
    if (word("ROW") && symbol("(", 1)) {
      take();
      take();
      auto e = node(ExprKind::row, offset);
      do e->args.push_back(expression());
      while (symbol(",") && (take(), true));
      expect_symbol(")");
      return e;
    }
    if (one_of(t.value, niladic_functions)) {
      auto e = node(ExprKind::call, offset, upper(take().value));
      if (symbol("(")) call_arguments(*e);
      return e;
    }
    if (symbol("(", 1)) {
      if (is_reserved(t.value) && !one_of(t.value, reserved_functions)) fail(t);
      auto e = node(ExprKind::call, offset, upper(take().value));
      call_arguments(*e);
      return e;
    }
    if (is_reserved(t.value)) fail(t);
    return column();
  }

  void call_arguments(Expr& call) {
    expect_symbol("(");
    if (symbol(")")) {
      take();
      return;
    }
    if (accept_word("DISTINCT")) call.distinct = true;
    if (!call.distinct && symbol("*") && symbol(")", 1)) {
      take();
      take();
      call.star = true;
      return;
    }
    do call.args.push_back(expression());
    while (symbol(",") && (take(), true));
    expect_symbol(")");
  }

  ExprPtr column() {
    auto e = node(ExprKind::column, peek().offset);
    e->path.push_back(take().value);
    while (e->path.size() < 3 && symbol(".")) {
      take();
      e->path.push_back(name_after_dot());
    }
    return e;
  }

  ExprPtr system_variable() {
    auto e = node(ExprKind::system_variable, take().offset);
    if (peek().kind == TokenKind::word && one_of(peek().value, variable_scopes) && symbol(".", 1)) {
      e->scope = upper(take().value);
      take();
    }
    e->value = name_after_dot();
    return e;
  }

  ExprPtr parenthesised() {
    const auto offset = take().offset;
    auto first = expression();
    if (!symbol(",")) {
      expect_symbol(")");
      return first;
    }
    auto e = node(ExprKind::row, offset);
    e->args.push_back(std::move(first));
    while (symbol(",")) {
      take();
      e->args.push_back(expression());
    }
    expect_symbol(")");
    return e;
  }

  ExprPtr case_when() {
    auto e = node(ExprKind::case_when, take().offset);
    if (!word("WHEN")) e->operand = expression();
    if (!word("WHEN")) fail(peek());
    while (accept_word("WHEN")) {
      e->args.push_back(expression());
      expect_word("THEN");
      e->args.push_back(expression());
    }
    if (accept_word("ELSE")) e->otherwise = expression();
    expect_word("END");
    return e;
  }

  std::string_view _text;
  const ParserOptions& _options;
  Lexer _lexer;
  std::deque<Token> _ahead;
  size_t _last_end = 0;
  size_t _depth = 0;
};

std::string quote(std::string_view value) {
  std::string out = "'";
  for (const char c : value) {
    if (c == '\'')
      out += "''";
    else
      out.push_back(c);
  }
  return out + "'";
}

std::string hex_text(std::string_view bytes) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string out = "X'";
  for (const char c : bytes) {
    const auto b = static_cast<uint8_t>(c);
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0xf]);
  }
  return out + "'";
}

}  // namespace

std::string to_string(const Expr& e) {
  const auto args = [&](size_t from) {
    std::string out;
    for (size_t i = from; i < e.args.size(); ++i) out += " " + to_string(*e.args[i]);
    return out;
  };
  const auto negatable = [&](const char* name) { return std::string(e.negated ? "NOT_" : "") + name; };
  switch (e.kind) {
    case ExprKind::null:
      return "NULL";
    case ExprKind::boolean:
    case ExprKind::integer:
    case ExprKind::decimal:
    case ExprKind::floating:
    case ExprKind::keyword_value:
      return e.value;
    case ExprKind::string:
      if (e.charset == "national") return "N" + quote(e.value);
      return (e.charset.empty() ? "" : "_" + e.charset) + quote(e.value);
    case ExprKind::hex:
    case ExprKind::bits:
      return hex_text(e.value);
    case ExprKind::column: {
      std::string out;
      for (size_t i = 0; i < e.path.size(); ++i) out += (i ? "." : "") + e.path[i];
      return out;
    }
    case ExprKind::user_variable:
      return "@" + e.value;
    case ExprKind::system_variable:
      return "@@" + (e.scope.empty() ? "" : e.scope + ".") + e.value;
    case ExprKind::placeholder:
      return "?";
    case ExprKind::default_value:
      return "DEFAULT";
    case ExprKind::unary:
    case ExprKind::binary:
      return "(" + e.value + args(0) + ")";
    case ExprKind::is:
      return std::string("(") + (e.negated ? "IS_NOT" : "IS") + args(0) + " " + e.value + ")";
    case ExprKind::in:
      return "(" + negatable("IN") + args(0) + ")";
    case ExprKind::between:
      return "(" + negatable("BETWEEN") + args(0) + ")";
    case ExprKind::like:
      return "(" + negatable("LIKE") + args(0) + ")";
    case ExprKind::regexp:
      return "(" + negatable("REGEXP") + args(0) + ")";
    case ExprKind::call:
      return "(" + e.value + (e.distinct ? " DISTINCT" : "") + (e.star ? " *" : "") + args(0) + ")";
    case ExprKind::case_when: {
      std::string out = "(CASE";
      if (e.operand) out += " " + to_string(*e.operand);
      for (size_t i = 0; i + 1 < e.args.size(); i += 2) out += " (WHEN " + to_string(*e.args[i]) + " " + to_string(*e.args[i + 1]) + ")";
      if (e.otherwise) out += " (ELSE " + to_string(*e.otherwise) + ")";
      return out + ")";
    }
    case ExprKind::row:
      return "(ROW" + args(0) + ")";
    case ExprKind::collate:
      return "(COLLATE" + args(0) + " " + e.value + ")";
    case ExprKind::assign:
      return "(:=" + args(0) + ")";
  }
  return "?";
}

std::vector<Statement> parse(std::string_view text, const ParserOptions& options) { return Parser(text, options).statements(); }

}  // namespace kvasir::sql
