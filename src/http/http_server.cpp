#include "http/http_server.h"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

#include "sql/ast.h"
#include "sql/parser.h"

namespace dbengine {

namespace {

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string UrlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = HexDigit(s[i + 1]);
      int lo = HexDigit(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += (s[i] == '+') ? ' ' : s[i];
  }
  return out;
}

// Extracts and URL-decodes the value of `key` from a query string like
// "sql=SELECT+1&other=x". Returns "" if not present.
std::string GetQueryParam(const std::string& query, const std::string& key) {
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    size_t eq = pair.find('=');
    if (eq != std::string::npos && pair.substr(0, eq) == key) {
      return UrlDecode(pair.substr(eq + 1));
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return "";
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::string QueryResultToJson(const QueryResult& result) {
  std::string out = "{\"columns\":[";
  for (size_t i = 0; i < result.column_names.size(); ++i) {
    if (i > 0) out += ",";
    out += "\"" + JsonEscape(result.column_names[i]) + "\"";
  }
  out += "],\"rows\":[";
  for (size_t r = 0; r < result.rows.size(); ++r) {
    if (r > 0) out += ",";
    out += "[";
    const std::vector<Value>& row = result.rows[r];
    for (size_t c = 0; c < row.size(); ++c) {
      if (c > 0) out += ",";
      if (std::holds_alternative<int64_t>(row[c])) {
        out += std::to_string(std::get<int64_t>(row[c]));
      } else {
        out += "\"" + JsonEscape(std::get<std::string>(row[c])) + "\"";
      }
    }
    out += "]";
  }
  out += "],\"rows_affected\":" + std::to_string(result.rows_affected);
  out += ",\"message\":\"" + JsonEscape(result.message) + "\"}";
  return out;
}

std::string JsonError(const std::string& message) { return "{\"error\":\"" + JsonEscape(message) + "\"}"; }

struct RequestLine {
  std::string method;
  std::string path;
  std::string query;
};

// Parses just the first line ("GET /query?sql=... HTTP/1.1"); headers are
// read (to find the blank-line terminator) but otherwise ignored — this
// server is GET-only and never needs a request body.
RequestLine ParseRequestLine(const std::string& request) {
  RequestLine result;
  size_t line_end = request.find("\r\n");
  std::string line = request.substr(0, line_end);

  size_t sp1 = line.find(' ');
  size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return result;

  result.method = line.substr(0, sp1);
  std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);

  size_t q = target.find('?');
  if (q == std::string::npos) {
    result.path = target;
  } else {
    result.path = target.substr(0, q);
    result.query = target.substr(q + 1);
  }
  return result;
}

std::string ReadRequestHeaders(int fd) {
  std::string buf;
  char chunk[4096];
  while (buf.find("\r\n\r\n") == std::string::npos) {
    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n <= 0) break;
    buf.append(chunk, static_cast<size_t>(n));
    if (buf.size() > 64 * 1024) break;  // guard against unbounded/malformed input
  }
  return buf;
}

void WriteResponse(int fd, int status, const std::string& content_type, const std::string& body) {
  const char* status_text = "Error";
  switch (status) {
    case 200:
      status_text = "OK";
      break;
    case 400:
      status_text = "Bad Request";
      break;
    case 404:
      status_text = "Not Found";
      break;
    case 405:
      status_text = "Method Not Allowed";
      break;
    default:
      break;
  }

  std::string response = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n";
  response += "Content-Type: " + content_type + "\r\n";
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  response += "Connection: close\r\n\r\n";
  response += body;

  size_t sent = 0;
  while (sent < response.size()) {
    ssize_t n = write(fd, response.data() + sent, response.size() - sent);
    if (n <= 0) break;
    sent += static_cast<size_t>(n);
  }
}

}  // namespace

HttpServer::HttpServer(Database* db, int port) : db_(db) {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::runtime_error("HttpServer: socket() failed");

  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<uint16_t>(port));

  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(listen_fd_);
    throw std::runtime_error("HttpServer: bind() failed on port " + std::to_string(port));
  }
  if (listen(listen_fd_, /*backlog=*/16) < 0) {
    close(listen_fd_);
    throw std::runtime_error("HttpServer: listen() failed");
  }

  sockaddr_in bound{};
  socklen_t bound_len = sizeof(bound);
  if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
    bound_port_ = ntohs(bound.sin_port);
  }
}

HttpServer::~HttpServer() {
  if (listen_fd_ >= 0) close(listen_fd_);
}

void HttpServer::Run() {
  while (running_) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    int ready = poll(&pfd, 1, /*timeout_ms=*/100);
    if (ready <= 0) continue;  // timeout, or interrupted — recheck running_

    int client_fd = accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) continue;
    HandleConnection(client_fd);
    close(client_fd);
  }
}

void HttpServer::Stop() { running_ = false; }

void HttpServer::HandleConnection(int client_fd) {
  std::string request = ReadRequestHeaders(client_fd);
  RequestLine req = ParseRequestLine(request);

  if (req.method.empty()) {
    WriteResponse(client_fd, 400, "application/json", JsonError("malformed request"));
    return;
  }
  if (req.method != "GET") {
    WriteResponse(client_fd, 405, "application/json", JsonError("only GET is supported"));
    return;
  }

  if (req.path == "/health") {
    WriteResponse(client_fd, 200, "text/plain", "ok");
    return;
  }

  if (req.path == "/query") {
    std::string sql = GetQueryParam(req.query, "sql");
    if (sql.empty()) {
      WriteResponse(client_fd, 400, "application/json", JsonError("missing 'sql' query parameter"));
      return;
    }
    try {
      Statement stmt = Parser::Parse(sql);
      if (!std::holds_alternative<SelectStmt>(stmt)) {
        WriteResponse(client_fd, 400, "application/json",
                      JsonError("only SELECT statements are allowed (read-only API)"));
        return;
      }
      QueryResult result = db_->Execute(sql);
      WriteResponse(client_fd, 200, "application/json", QueryResultToJson(result));
    } catch (const std::exception& e) {
      WriteResponse(client_fd, 400, "application/json", JsonError(e.what()));
    }
    return;
  }

  WriteResponse(client_fd, 404, "application/json", JsonError("not found"));
}

}  // namespace dbengine
