#pragma once

#include <string>

#include "sql/ast.h"

namespace dbengine {

// A small recursive-descent parser over the grammar this project's SQL
// layer supports:
//
//   CREATE TABLE name (col type (, col type)*)
//   INSERT INTO name VALUES (literal (, literal)*)
//   SELECT (* | col (, col)*) FROM name (WHERE comparison (AND comparison)*)?
//   DELETE FROM name (WHERE comparison (AND comparison)*)?
//   comparison := col (= | < | <= | > | >=) literal
//   type := INTEGER | TEXT
//
// Keywords are matched case-insensitively. A trailing ';' is optional.
// Throws SqlException on any syntax error.
class Parser {
 public:
  static Statement Parse(const std::string& sql);
};

}  // namespace dbengine
