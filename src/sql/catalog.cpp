#include "sql/catalog.h"

#include <cstring>

#include "sql/sql_exception.h"

namespace dbengine {

int TableSchema::ColumnIndex(const std::string& col_name) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].name == col_name) return static_cast<int>(i);
  }
  return -1;
}

namespace {

void AppendInteger(std::string* out, int64_t v) {
  char buf[sizeof(int64_t)];
  std::memcpy(buf, &v, sizeof(v));
  out->append(buf, sizeof(buf));
}

void AppendText(std::string* out, const std::string& s) {
  if (s.size() > 0xFFFF) throw SqlException("TEXT value too long (max 65535 bytes)");
  uint16_t len = static_cast<uint16_t>(s.size());
  char buf[sizeof(uint16_t)];
  std::memcpy(buf, &len, sizeof(len));
  out->append(buf, sizeof(buf));
  out->append(s);
}

int64_t ReadInteger(const std::string& payload, size_t* offset) {
  if (*offset + sizeof(int64_t) > payload.size()) {
    throw SqlException("corrupt row: truncated INTEGER field");
  }
  int64_t v;
  std::memcpy(&v, payload.data() + *offset, sizeof(v));
  *offset += sizeof(v);
  return v;
}

std::string ReadText(const std::string& payload, size_t* offset) {
  if (*offset + sizeof(uint16_t) > payload.size()) {
    throw SqlException("corrupt row: truncated TEXT length prefix");
  }
  uint16_t len;
  std::memcpy(&len, payload.data() + *offset, sizeof(len));
  *offset += sizeof(len);
  if (*offset + len > payload.size()) {
    throw SqlException("corrupt row: truncated TEXT payload");
  }
  std::string s = payload.substr(*offset, len);
  *offset += len;
  return s;
}

}  // namespace

std::string EncodeRow(const TableSchema& schema, const std::vector<Value>& values) {
  if (values.size() != schema.columns.size()) {
    throw SqlException("expected " + std::to_string(schema.columns.size()) + " values for table '" +
                        schema.name + "', got " + std::to_string(values.size()));
  }

  std::string payload;
  for (size_t i = 1; i < schema.columns.size(); ++i) {
    const ColumnDef& col = schema.columns[i];
    const Value& v = values[i];
    if (col.type == ColumnType::INTEGER) {
      if (!std::holds_alternative<int64_t>(v)) {
        throw SqlException("column '" + col.name + "' expects an INTEGER value");
      }
      AppendInteger(&payload, std::get<int64_t>(v));
    } else {
      if (!std::holds_alternative<std::string>(v)) {
        throw SqlException("column '" + col.name + "' expects a TEXT value");
      }
      AppendText(&payload, std::get<std::string>(v));
    }
  }

  if (payload.size() > MAX_VALUE_SIZE) {
    throw SqlException("row for table '" + schema.name + "' is " + std::to_string(payload.size()) +
                        " bytes, exceeding the " + std::to_string(MAX_VALUE_SIZE) +
                        "-byte row cap (see DESIGN.md — MAX_VALUE_SIZE is shared with the storage engines)");
  }
  return payload;
}

std::vector<Value> DecodeRow(const TableSchema& schema, int64_t primary_key, const std::string& payload) {
  std::vector<Value> values;
  values.reserve(schema.columns.size());
  values.emplace_back(primary_key);

  size_t offset = 0;
  for (size_t i = 1; i < schema.columns.size(); ++i) {
    const ColumnDef& col = schema.columns[i];
    if (col.type == ColumnType::INTEGER) {
      values.emplace_back(ReadInteger(payload, &offset));
    } else {
      values.emplace_back(ReadText(payload, &offset));
    }
  }
  return values;
}

const TableSchema& Catalog::CreateTable(const std::string& name, const std::vector<ColumnDefAst>& columns) {
  if (tables_.count(name) > 0) {
    throw SqlException("table '" + name + "' already exists");
  }
  if (next_table_id_ >= kMaxTables) {
    throw SqlException("table id space exhausted");
  }

  TableSchema schema;
  schema.name = name;
  schema.table_id = next_table_id_++;
  for (const ColumnDefAst& c : columns) {
    schema.columns.push_back(ColumnDef{c.name, c.type});
  }

  auto [it, inserted] = tables_.emplace(name, std::move(schema));
  return it->second;
}

const TableSchema& Catalog::GetTable(const std::string& name) const {
  auto it = tables_.find(name);
  if (it == tables_.end()) {
    throw SqlException("no such table: '" + name + "'");
  }
  return it->second;
}

bool Catalog::HasTable(const std::string& name) const { return tables_.count(name) > 0; }

}  // namespace dbengine
