#include "sql/lexer.h"

#include <cctype>

#include "sql/sql_exception.h"

namespace dbengine {

std::vector<Token> Lexer::Tokenize(const std::string& sql) {
  std::vector<Token> tokens;
  size_t i = 0;
  size_t n = sql.size();

  while (i < n) {
    char c = sql[i];

    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }

    if (c == '(') {
      tokens.push_back({TokenType::LPAREN, "("});
      ++i;
      continue;
    }
    if (c == ')') {
      tokens.push_back({TokenType::RPAREN, ")"});
      ++i;
      continue;
    }
    if (c == ',') {
      tokens.push_back({TokenType::COMMA, ","});
      ++i;
      continue;
    }
    if (c == ';') {
      tokens.push_back({TokenType::SEMICOLON, ";"});
      ++i;
      continue;
    }
    if (c == '*') {
      tokens.push_back({TokenType::STAR, "*"});
      ++i;
      continue;
    }
    if (c == '=') {
      tokens.push_back({TokenType::EQ, "="});
      ++i;
      continue;
    }
    if (c == '<') {
      if (i + 1 < n && sql[i + 1] == '=') {
        tokens.push_back({TokenType::LE, "<="});
        i += 2;
      } else {
        tokens.push_back({TokenType::LT, "<"});
        ++i;
      }
      continue;
    }
    if (c == '>') {
      if (i + 1 < n && sql[i + 1] == '=') {
        tokens.push_back({TokenType::GE, ">="});
        i += 2;
      } else {
        tokens.push_back({TokenType::GT, ">"});
        ++i;
      }
      continue;
    }

    if (c == '\'') {
      size_t start = i + 1;
      size_t end = sql.find('\'', start);
      if (end == std::string::npos) {
        throw SqlException("unterminated string literal starting at position " + std::to_string(i));
      }
      tokens.push_back({TokenType::STRING_LITERAL, sql.substr(start, end - start)});
      i = end + 1;
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && i + 1 < n && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
      size_t start = i;
      ++i;
      while (i < n && std::isdigit(static_cast<unsigned char>(sql[i]))) ++i;
      std::string text = sql.substr(start, i - start);
      Token tok{TokenType::INTEGER_LITERAL, text};
      tok.int_value = std::stoll(text);
      tokens.push_back(tok);
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t start = i;
      ++i;
      while (i < n && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) ++i;
      tokens.push_back({TokenType::IDENTIFIER, sql.substr(start, i - start)});
      continue;
    }

    throw SqlException(std::string("unexpected character '") + c + "' at position " + std::to_string(i));
  }

  tokens.push_back({TokenType::END, ""});
  return tokens;
}

}  // namespace dbengine
