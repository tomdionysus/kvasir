//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual, "Language Structure":
// - String Literals: '...' and "..."; a quote inside written twice; the
//   backslash escapes \0 \' \" \b \n \r \t \Z \\ \% \_ (the last two keep
//   their backslash), a backslash before any other character ignored;
//   NO_BACKSLASH_ESCAPES turns them off; ANSI_QUOTES makes "..." an
//   identifier; N'...' is a national string.
// - Numeric Literals: 1, .2, 3.4, -5 (a minus and a number), 1.2E3, 1.2E-3.
// - Hexadecimal Literals: X'01AF' (an even count of digits), 0x01AF (any
//   count, padded on the left), x'' empty; 0X is not one.
// - Bit-Value Literals: B'01', 0b01; 0B is not one.
// - Schema Object Names: unquoted [0-9a-zA-Z$_] and U+0080 up; may begin
//   with a digit but not be digits alone; `quoted`, a backtick inside
//   written twice; after "ident." digits begin a name.
// - User-Defined Variables: @name with [0-9a-zA-Z._$] and @'...', @"...",
//   @`...`.
// - Comments: # to the end of the line; "-- " needs a space or control
//   character after the dashes; /* */; /*! */ is executed, /*!NNNNN */ when
//   the server's version is at least NNNNN; /*+ */ is an optimizer hint
//   after SELECT, INSERT, REPLACE, UPDATE and DELETE (and, Kvasir's, COMMIT,
//   for ack_level).
// - ER_PARSE_ERROR 1064 with its message.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "kvasir/sql/lexer.h"

namespace kvasir::sql {
namespace {

using K = TokenKind;

std::vector<Token> lex(std::string_view text, LexerOptions options = {}) {
  Lexer lexer(text, options);
  std::vector<Token> out;
  for (;;) {
    auto token = lexer.next();
    if (token.kind == K::end) break;
    out.push_back(std::move(token));
    // Never more tokens than bytes: past that the lexer is not advancing.
    if (out.size() > text.size()) {
      ADD_FAILURE() << "the lexer does not end";
      break;
    }
  }
  return out;
}

// "kind:value" per token, for compact expectations.
std::vector<std::string> shape(std::string_view text, LexerOptions options = {}) {
  static const char* names[] = {"end", "word", "qid", "str", "nstr", "int", "dec", "float", "hex", "bits", "var", "?", "hint", "sym"};
  std::vector<std::string> out;
  for (const auto& t : lex(text, options)) out.push_back(std::string(names[static_cast<int>(t.kind)]) + ":" + t.value);
  return out;
}

using V = std::vector<std::string>;

SyntaxError error_of(std::string_view text, LexerOptions options = {}) {
  try {
    lex(text, options);
  } catch (const SyntaxError& e) {
    return e;
  }
  ADD_FAILURE() << "no syntax error for: " << text;
  return SyntaxError(text, 0, 1);
}

TEST(SqlLexer, AnEmptyTextIsTheEnd) {
  Lexer lexer("  \t\n ");
  EXPECT_EQ(lexer.next().kind, K::end);
  EXPECT_EQ(lexer.next().kind, K::end);
}

TEST(SqlLexer, WordsSymbolsAndNumbers) {
  EXPECT_EQ(shape("SELECT a, b FROM t WHERE x = 1;"),
            (V{"word:SELECT", "word:a", "sym:,", "word:b", "word:FROM", "word:t", "word:WHERE", "word:x", "sym:=", "int:1", "sym:;"}));
}

TEST(SqlLexer, TheVersionCommentQuery) {
  EXPECT_EQ(shape("select @@version_comment limit 1"), (V{"word:select", "sym:@@", "word:version_comment", "word:limit", "int:1"}));
}

TEST(SqlLexer, SystemVariablesWithAScope) {
  EXPECT_EQ(shape("SET @@SESSION.autocommit=0"), (V{"word:SET", "sym:@@", "word:SESSION", "sym:.", "word:autocommit", "sym:=", "int:0"}));
}

TEST(SqlLexer, TokensKnowWhereTheyStand) {
  const auto tokens = lex("a\n  bc\n\n'd'");
  ASSERT_EQ(tokens.size(), 3u);
  EXPECT_EQ(tokens[0].offset, 0u);
  EXPECT_EQ(tokens[0].line, 1u);
  EXPECT_EQ(tokens[1].offset, 4u);
  EXPECT_EQ(tokens[1].length, 2u);
  EXPECT_EQ(tokens[1].line, 2u);
  EXPECT_EQ(tokens[2].offset, 8u);
  EXPECT_EQ(tokens[2].length, 3u);
  EXPECT_EQ(tokens[2].line, 4u);
}

TEST(SqlLexer, StringsInEitherQuote) {
  EXPECT_EQ(shape("'a string' \"another\""), (V{"str:a string", "str:another"}));
  EXPECT_EQ(shape("''"), (V{"str:"}));
}

TEST(SqlLexer, AQuoteWrittenTwice) {
  EXPECT_EQ(shape("'hel''lo' \"hel\"\"lo\" 'he\"llo' \"he'llo\""), (V{"str:hel'lo", "str:hel\"lo", "str:he\"llo", "str:he'llo"}));
}

TEST(SqlLexer, TheManualsBackslashEscapes) {
  const auto tokens = lex(R"('\0\'\"\b\n\r\t\Z\\\%\_\x')");
  ASSERT_EQ(tokens.size(), 1u);
  EXPECT_EQ(tokens[0].value, std::string("\0'\"\b\n\r\t\x1a\\\\%\\_x", 14));
}

TEST(SqlLexer, NoBackslashEscapes) {
  LexerOptions options;
  options.no_backslash_escapes = true;
  EXPECT_EQ(shape(R"('a\nb' 'c\')", options), (V{R"(str:a\nb)", R"(str:c\)"}));
}

TEST(SqlLexer, AnsiQuotesMakeDoubleQuotesAName) {
  LexerOptions options;
  options.ansi_quotes = true;
  EXPECT_EQ(shape("SELECT \"a\"\"b\" FROM 'x'", options), (V{"word:SELECT", "qid:a\"b", "word:FROM", "str:x"}));
}

TEST(SqlLexer, NationalStrings) { EXPECT_EQ(shape("N'abc' n'd' N 'e'"), (V{"nstr:abc", "nstr:d", "word:N", "str:e"})); }

TEST(SqlLexer, AnIntroducerIsAWordBeforeAString) { EXPECT_EQ(shape("_utf8mb4'abc'"), (V{"word:_utf8mb4", "str:abc"})); }

TEST(SqlLexer, NumericLiterals) {
  EXPECT_EQ(shape("1 .2 3.4 -5 -6.78 +9.10 1.2E3 1.2E-3 4e+2 5. 007"), (V{"int:1", "dec:.2", "dec:3.4", "sym:-", "int:5", "sym:-", "dec:6.78", "sym:+",
                                                                          "dec:9.10", "float:1.2E3", "float:1.2E-3", "float:4e+2", "dec:5.", "int:007"}));
}

TEST(SqlLexer, HexLiterals) {
  EXPECT_EQ(shape("X'01AF' x'01af' X'' 0x01AF 0xaBc"), (V{"hex:\x01\xaf", "hex:\x01\xaf", "hex:", "hex:\x01\xaf", std::string("hex:\x0a\xbc", 6)}));
}

TEST(SqlLexer, HexLiteralsThatAreNot) {
  EXPECT_EQ(shape("0X1A 0xG"), (V{"word:0X1A", "word:0xG"}));
  EXPECT_EQ(error_of("X'0G'").offset(), 0u);
  EXPECT_EQ(error_of("SELECT X'0'").offset(), 7u);
}

TEST(SqlLexer, BitLiterals) {
  EXPECT_EQ(shape("b'01' B'1000001' 0b1100001 b''"), (V{"bits:\x01", "bits:A", "bits:a", "bits:"}));
  EXPECT_EQ(shape("0b111111111"), (V{std::string("bits:\x01\xff", 7)}));
  EXPECT_EQ(shape("0B1 0b2"), (V{"word:0B1", "word:0b2"}));
  EXPECT_THROW(lex("b'012'"), SyntaxError);
}

TEST(SqlLexer, NamesMayBeginWithADigit) { EXPECT_EQ(shape("1a a1 $x _y 1e 12abc"), (V{"word:1a", "word:a1", "word:$x", "word:_y", "word:1e", "word:12abc"})); }

TEST(SqlLexer, DigitsAfterANameAndADotAreAName) {
  EXPECT_EQ(shape("t.1e3 db.2col `t`.3"), (V{"word:t", "sym:.", "word:1e3", "word:db", "sym:.", "word:2col", "qid:t", "sym:.", "word:3"}));
  EXPECT_EQ(shape("t .5"), (V{"word:t", "dec:.5"}));
}

// Found by the lexer's fuzz target: after "name." a second dot begins no
// name, and no token is ever empty.
TEST(SqlLexer, ADotAfterANamesDotIsNoName) {
  EXPECT_EQ(shape("t..5"), (V{"word:t", "sym:.", "dec:.5"}));
  for (const auto& token : lex("nn..8!=..6")) EXPECT_GT(token.length, 0u);
}

TEST(SqlLexer, NamesBeyondAscii) { EXPECT_EQ(shape("SELECT café, 名前"), (V{"word:SELECT", "word:café", "sym:,", "word:名前"})); }

TEST(SqlLexer, BytesThatAreNotUtf8AreRefusedOutsideStrings) {
  EXPECT_EQ(error_of("SELECT a\xff").offset(), 8u);
  EXPECT_EQ(error_of("SELECT \xc3").offset(), 7u);
  EXPECT_EQ(shape("'\xff'"), (V{"str:\xff"}));
}

TEST(SqlLexer, BacktickNames) {
  EXPECT_EQ(shape("`select` `a``b` `with space` ``"), (V{"qid:select", "qid:a`b", "qid:with space", "qid:"}));
  EXPECT_EQ(shape("\"x\""), (V{"str:x"}));
}

TEST(SqlLexer, UserVariables) {
  EXPECT_EQ(shape("@a @a.b$c @'my-var' @\"x y\" @`z``w` @1"), (V{"var:a", "var:a.b$c", "var:my-var", "var:x y", "var:z`w", "var:1"}));
  EXPECT_EQ(shape("SET @x := 1"), (V{"word:SET", "var:x", "sym::=", "int:1"}));
}

TEST(SqlLexer, ALoneAtIsRefused) {
  EXPECT_THROW(lex("SELECT @"), SyntaxError);
  EXPECT_THROW(lex("SELECT @ a"), SyntaxError);
}

TEST(SqlLexer, Operators) {
  EXPECT_EQ(
      shape("<=> <> != <= >= << >> := && || -> ->> ( ) + - * / % ^ & | ~ ! < > = { } :"),
      (V{"sym:<=>", "sym:<>", "sym:!=", "sym:<=", "sym:>=", "sym:<<", "sym:>>", "sym::=", "sym:&&", "sym:||", "sym:->", "sym:->>", "sym:(", "sym:)", "sym:+",
         "sym:-",   "sym:*",  "sym:/",  "sym:%",  "sym:^",  "sym:&",  "sym:|",  "sym:~",  "sym:!",  "sym:<",  "sym:>",  "sym:=",   "sym:{", "sym:}", "sym::"}));
  EXPECT_EQ(shape("a<=>b"), (V{"word:a", "sym:<=>", "word:b"}));
  EXPECT_EQ(shape("col->>'$.x'"), (V{"word:col", "sym:->>", "str:$.x"}));
}

TEST(SqlLexer, Placeholders) { EXPECT_EQ(shape("WHERE a = ? AND b = ?"), (V{"word:WHERE", "word:a", "sym:=", "?:?", "word:AND", "word:b", "sym:=", "?:?"})); }

TEST(SqlLexer, HashAndDashComments) {
  EXPECT_EQ(shape("SELECT 1 # a comment\n+ 2 -- another\n-- \n--\t\n- 3 --"), (V{"word:SELECT", "int:1", "sym:+", "int:2", "sym:-", "int:3"}));
}

TEST(SqlLexer, TwoDashesWithoutSpaceAreTwoMinuses) { EXPECT_EQ(shape("1--1"), (V{"int:1", "sym:-", "sym:-", "int:1"})); }

TEST(SqlLexer, BlockComments) {
  EXPECT_EQ(shape("SELECT /* a\nmultiline */ 1 /**/ + /* * / */ 2"), (V{"word:SELECT", "int:1", "sym:+", "int:2"}));
  const auto tokens = lex("/* x\n y */ a");
  ASSERT_EQ(tokens.size(), 1u);
  EXPECT_EQ(tokens[0].line, 2u);
}

TEST(SqlLexer, AnUnterminatedCommentIsRefused) {
  const auto e = error_of("SELECT 1 /* never");
  EXPECT_EQ(e.offset(), 9u);
}

TEST(SqlLexer, ExecutableComments) {
  EXPECT_EQ(shape("SELECT /*! STRAIGHT_JOIN */ a"), (V{"word:SELECT", "word:STRAIGHT_JOIN", "word:a"}));
  EXPECT_EQ(shape("CREATE TABLE t (a INT) /*!50110 KEY_BLOCK_SIZE=1024 */"),
            (V{"word:CREATE", "word:TABLE", "word:t", "sym:(", "word:a", "word:INT", "sym:)", "word:KEY_BLOCK_SIZE", "sym:=", "int:1024"}));
  EXPECT_EQ(shape("SELECT 1 /*!90000 + 2 */"), (V{"word:SELECT", "int:1"}));
  EXPECT_EQ(shape("SELECT 1 /*!80400 + 2 */"), (V{"word:SELECT", "int:1", "sym:+", "int:2"}));
  EXPECT_EQ(shape("SELECT 1 /*!80401 + 2 */"), (V{"word:SELECT", "int:1"}));
  EXPECT_EQ(shape("/*!40101 SET @x=1*/;"), (V{"word:SET", "var:x", "sym:=", "int:1", "sym:;"}));
}

TEST(SqlLexer, AnExecutableCommentMustClose) { EXPECT_THROW(lex("SELECT /*! 1"), SyntaxError); }

TEST(SqlLexer, HintsFollowAStatementsFirstKeyword) {
  EXPECT_EQ(shape("SELECT /*+ MAX_EXECUTION_TIME(1000) */ a"), (V{"word:SELECT", "hint: MAX_EXECUTION_TIME(1000) ", "word:a"}));
  EXPECT_EQ(shape("commit /*+ SET_VAR(ack_level='ephemeral') */"), (V{"word:commit", "hint: SET_VAR(ack_level='ephemeral') "}));
  // Elsewhere it is a comment.
  EXPECT_EQ(shape("SELECT a /*+ NO_ICP(t) */ FROM t"), (V{"word:SELECT", "word:a", "word:FROM", "word:t"}));
  EXPECT_EQ(shape("/*+ x */ SELECT 1"), (V{"word:SELECT", "int:1"}));
}

TEST(SqlLexer, UnterminatedStringsAndNamesAreRefused) {
  EXPECT_EQ(error_of("SELECT 'abc").offset(), 7u);
  EXPECT_EQ(error_of("SELECT \"abc").offset(), 7u);
  EXPECT_EQ(error_of("SELECT `abc").offset(), 7u);
  EXPECT_EQ(error_of("SELECT 'abc\\'").offset(), 7u);
  EXPECT_EQ(error_of("SELECT @'abc").offset(), 7u);
}

TEST(SqlLexer, CharactersNoTokenBeginsWithAreRefused) {
  EXPECT_EQ(error_of("SELECT \\").offset(), 7u);
  EXPECT_EQ(error_of("SELECT [a]").offset(), 7u);
  EXPECT_EQ(error_of(std::string_view("SELECT \0", 8)).offset(), 7u);
}

TEST(SqlLexer, TheParseErrorIsMysqls) {
  const auto e = error_of("SELECT 1\nFROM 'abc");
  EXPECT_EQ(e.line(), 2u);
  EXPECT_STREQ(e.what(),
               "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near ''abc' "
               "at line 2");
}

TEST(SqlLexer, TheParseErrorQuotesAtMost80Bytes) {
  const std::string tail(100, 'x');
  const SyntaxError e(tail, 0, 1);
  EXPECT_EQ(std::string(e.what()),
            "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use near '" +
                std::string(80, 'x') + "' at line 1");
  // Not through the middle of a character.
  const std::string wide = std::string(79, 'x') + "é";
  const SyntaxError cut(wide, 0, 1);
  EXPECT_NE(std::string(cut.what()).find("near '" + std::string(79, 'x') + "' at"), std::string::npos);
}

TEST(SqlLexer, AtTheEndTheErrorQuotesNothing) {
  const SyntaxError e("SELECT", 6, 1);
  EXPECT_NE(std::string(e.what()).find("near '' at line 1"), std::string::npos);
}

TEST(SqlLexer, ALongTextIsLexedInOnePass) {
  std::string text = "SELECT ";
  for (int i = 0; i < 20000; ++i) text += "a" + std::to_string(i) + ",";
  text += "1";
  EXPECT_EQ(lex(text).size(), 1u + 20000 * 2 + 1);
}

}  // namespace
}  // namespace kvasir::sql
