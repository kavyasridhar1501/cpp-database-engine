#pragma once

#include <string>
#include <vector>

#include "sql/token.h"

namespace dbengine {

// Turns a SQL statement into a flat token stream, terminated by a single
// END token. Throws SqlException on an unrecognized character or an
// unterminated string literal.
class Lexer {
 public:
  static std::vector<Token> Tokenize(const std::string& sql);
};

}  // namespace dbengine
