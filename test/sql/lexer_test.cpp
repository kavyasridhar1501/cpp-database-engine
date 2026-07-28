#include "sql/lexer.h"

#include <gtest/gtest.h>

#include "sql/sql_exception.h"

namespace dbengine {
namespace {

TEST(LexerTest, TokenizesPunctuationAndOperators) {
  auto tokens = Lexer::Tokenize("(),;* = < <= > >=");
  std::vector<TokenType> types;
  for (const auto& t : tokens) types.push_back(t.type);
  std::vector<TokenType> expected = {
      TokenType::LPAREN, TokenType::RPAREN, TokenType::COMMA, TokenType::SEMICOLON,
      TokenType::STAR,   TokenType::EQ,     TokenType::LT,    TokenType::LE,
      TokenType::GT,     TokenType::GE,     TokenType::END,
  };
  EXPECT_EQ(types, expected);
}

TEST(LexerTest, TokenizesIdentifiersAndKeywordsUniformly) {
  auto tokens = Lexer::Tokenize("SELECT foo_bar FROM my_table");
  ASSERT_EQ(tokens.size(), 5u);  // 4 identifiers + END
  for (int i = 0; i < 4; ++i) EXPECT_EQ(tokens[i].type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[0].text, "SELECT");
  EXPECT_EQ(tokens[1].text, "foo_bar");
  EXPECT_EQ(tokens[3].text, "my_table");
}

TEST(LexerTest, TokenizesIntegerLiterals) {
  auto tokens = Lexer::Tokenize("42 -7 0");
  ASSERT_EQ(tokens.size(), 4u);
  EXPECT_EQ(tokens[0].type, TokenType::INTEGER_LITERAL);
  EXPECT_EQ(tokens[0].int_value, 42);
  EXPECT_EQ(tokens[1].int_value, -7);
  EXPECT_EQ(tokens[2].int_value, 0);
}

TEST(LexerTest, TokenizesStringLiterals) {
  auto tokens = Lexer::Tokenize("'hello world' ''");
  ASSERT_EQ(tokens.size(), 3u);
  EXPECT_EQ(tokens[0].type, TokenType::STRING_LITERAL);
  EXPECT_EQ(tokens[0].text, "hello world");
  EXPECT_EQ(tokens[1].text, "");
}

TEST(LexerTest, ThrowsOnUnterminatedString) {
  EXPECT_THROW(Lexer::Tokenize("'unterminated"), SqlException);
}

TEST(LexerTest, ThrowsOnUnexpectedCharacter) {
  EXPECT_THROW(Lexer::Tokenize("SELECT # FROM t"), SqlException);
}

}  // namespace
}  // namespace dbengine
