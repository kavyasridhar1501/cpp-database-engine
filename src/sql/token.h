#pragma once

#include <cstdint>
#include <string>

namespace dbengine {

enum class TokenType {
  IDENTIFIER,        // also carries keywords (CREATE, SELECT, ...) — the
                      // parser matches keyword text case-insensitively
                      // rather than the lexer special-casing each one.
  INTEGER_LITERAL,
  STRING_LITERAL,     // single-quoted; '' inside is not supported (tiny
                      // scope — see DESIGN.md)
  LPAREN,
  RPAREN,
  COMMA,
  SEMICOLON,
  STAR,
  EQ,
  LT,
  LE,
  GT,
  GE,
  END,
};

struct Token {
  TokenType type;
  std::string text;       // raw text for IDENTIFIER/STRING_LITERAL
  int64_t int_value = 0;  // populated for INTEGER_LITERAL
};

}  // namespace dbengine
