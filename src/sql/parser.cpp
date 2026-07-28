#include "sql/parser.h"

#include <algorithm>
#include <cctype>

#include "sql/lexer.h"
#include "sql/sql_exception.h"

namespace dbengine {

namespace {

std::string ToUpper(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                  [](unsigned char c) { return std::toupper(c); });
  return out;
}

class ParserImpl {
 public:
  explicit ParserImpl(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  Statement ParseStatement() {
    if (IsKeyword("CREATE")) return ParseCreateTable();
    if (IsKeyword("INSERT")) return ParseInsert();
    if (IsKeyword("SELECT")) return ParseSelect();
    if (IsKeyword("DELETE")) return ParseDelete();
    throw SqlException("expected CREATE, INSERT, SELECT, or DELETE, got '" + Peek().text + "'");
  }

  void ExpectEndOfStatement() {
    if (Peek().type == TokenType::SEMICOLON) Advance();
    if (Peek().type != TokenType::END) {
      throw SqlException("unexpected trailing input near '" + Peek().text + "'");
    }
  }

 private:
  const Token& Peek() const { return tokens_[pos_]; }
  const Token& Advance() { return tokens_[pos_++]; }

  bool IsKeyword(const std::string& kw) const {
    return Peek().type == TokenType::IDENTIFIER && ToUpper(Peek().text) == kw;
  }

  void ExpectKeyword(const std::string& kw) {
    if (!IsKeyword(kw)) {
      throw SqlException("expected keyword '" + kw + "', got '" + Peek().text + "'");
    }
    Advance();
  }

  Token ExpectType(TokenType type, const std::string& what) {
    if (Peek().type != type) {
      throw SqlException("expected " + what + ", got '" + Peek().text + "'");
    }
    return Advance();
  }

  std::string ExpectIdentifier() { return ExpectType(TokenType::IDENTIFIER, "identifier").text; }

  ColumnType ParseColumnType() {
    if (IsKeyword("INTEGER")) {
      Advance();
      return ColumnType::INTEGER;
    }
    if (IsKeyword("TEXT")) {
      Advance();
      return ColumnType::TEXT;
    }
    throw SqlException("expected column type (INTEGER or TEXT), got '" + Peek().text + "'");
  }

  Value ParseLiteral() {
    if (Peek().type == TokenType::INTEGER_LITERAL) {
      return Value(Advance().int_value);
    }
    if (Peek().type == TokenType::STRING_LITERAL) {
      return Value(Advance().text);
    }
    throw SqlException("expected a literal value, got '" + Peek().text + "'");
  }

  CompareOp ParseCompareOp() {
    switch (Peek().type) {
      case TokenType::EQ:
        Advance();
        return CompareOp::EQ;
      case TokenType::LT:
        Advance();
        return CompareOp::LT;
      case TokenType::LE:
        Advance();
        return CompareOp::LE;
      case TokenType::GT:
        Advance();
        return CompareOp::GT;
      case TokenType::GE:
        Advance();
        return CompareOp::GE;
      default:
        throw SqlException("expected a comparison operator (=, <, <=, >, >=), got '" + Peek().text + "'");
    }
  }

  Predicate ParseWhereClause() {
    Predicate predicate;
    if (!IsKeyword("WHERE")) return predicate;
    Advance();
    while (true) {
      std::string column = ExpectIdentifier();
      CompareOp op = ParseCompareOp();
      Value literal = ParseLiteral();
      predicate.push_back(Comparison{column, op, literal});
      if (IsKeyword("AND")) {
        Advance();
        continue;
      }
      break;
    }
    return predicate;
  }

  Statement ParseCreateTable() {
    Advance();  // CREATE
    ExpectKeyword("TABLE");
    std::string table_name = ExpectIdentifier();
    ExpectType(TokenType::LPAREN, "'('");

    CreateTableStmt stmt;
    stmt.table_name = table_name;
    while (true) {
      std::string col_name = ExpectIdentifier();
      ColumnType col_type = ParseColumnType();
      stmt.columns.push_back(ColumnDefAst{col_name, col_type});
      if (Peek().type == TokenType::COMMA) {
        Advance();
        continue;
      }
      break;
    }
    ExpectType(TokenType::RPAREN, "')'");
    if (stmt.columns.empty()) throw SqlException("CREATE TABLE needs at least one column");
    if (stmt.columns[0].type != ColumnType::INTEGER) {
      throw SqlException("the first column ('" + stmt.columns[0].name +
                          "') must be INTEGER — it is the table's primary key");
    }
    return stmt;
  }

  Statement ParseInsert() {
    Advance();  // INSERT
    ExpectKeyword("INTO");
    std::string table_name = ExpectIdentifier();
    ExpectKeyword("VALUES");
    ExpectType(TokenType::LPAREN, "'('");

    InsertStmt stmt;
    stmt.table_name = table_name;
    while (true) {
      stmt.values.push_back(ParseLiteral());
      if (Peek().type == TokenType::COMMA) {
        Advance();
        continue;
      }
      break;
    }
    ExpectType(TokenType::RPAREN, "')'");
    return stmt;
  }

  Statement ParseSelect() {
    Advance();  // SELECT
    SelectStmt stmt;
    if (Peek().type == TokenType::STAR) {
      Advance();
      stmt.select_star = true;
    } else {
      stmt.select_star = false;
      while (true) {
        stmt.columns.push_back(ExpectIdentifier());
        if (Peek().type == TokenType::COMMA) {
          Advance();
          continue;
        }
        break;
      }
    }
    ExpectKeyword("FROM");
    stmt.table_name = ExpectIdentifier();
    stmt.where = ParseWhereClause();
    return stmt;
  }

  Statement ParseDelete() {
    Advance();  // DELETE
    ExpectKeyword("FROM");
    DeleteStmt stmt;
    stmt.table_name = ExpectIdentifier();
    stmt.where = ParseWhereClause();
    return stmt;
  }

  std::vector<Token> tokens_;
  size_t pos_ = 0;
};

}  // namespace

Statement Parser::Parse(const std::string& sql) {
  ParserImpl parser(Lexer::Tokenize(sql));
  Statement stmt = parser.ParseStatement();
  parser.ExpectEndOfStatement();
  return stmt;
}

}  // namespace dbengine
