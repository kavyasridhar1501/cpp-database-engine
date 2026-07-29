#include "http/http_server.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace dbengine {
namespace {

struct HttpResponse {
  int status = 0;
  std::string body;
};

// Minimal raw-socket HTTP client; no keep-alive or chunked encoding since
// HttpServer never produces either.
HttpResponse SendRequest(int port, const std::string& method, const std::string& target) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  // Kernel queues the connection once HttpServer's constructor returns
  // (listen() already called), so this doesn't race Run()'s accept loop;
  // retry is just insurance for a loaded CI box.
  int rc = -1;
  for (int attempt = 0; attempt < 50 && rc != 0; ++attempt) {
    rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(rc, 0);

  std::string request = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  EXPECT_GE(write(fd, request.data(), request.size()), 0);

  std::string response;
  char chunk[4096];
  ssize_t n;
  while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
    response.append(chunk, static_cast<size_t>(n));
  }
  close(fd);

  HttpResponse result;
  size_t sp1 = response.find(' ');
  size_t sp2 = sp1 == std::string::npos ? std::string::npos : response.find(' ', sp1 + 1);
  if (sp1 != std::string::npos && sp2 != std::string::npos) {
    result.status = std::stoi(response.substr(sp1 + 1, sp2 - sp1 - 1));
  }
  size_t body_start = response.find("\r\n\r\n");
  if (body_start != std::string::npos) {
    result.body = response.substr(body_start + 4);
  }
  return result;
}

class HttpServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    file_path_ = (std::filesystem::temp_directory_path() /
                  ("dbengine_http_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db"))
                     .string();
    std::filesystem::remove(file_path_);

    db_ = std::make_unique<Database>(file_path_, EngineType::BTREE);
    db_->Execute("CREATE TABLE users (id INTEGER, name TEXT, age INTEGER)");
    db_->Execute("INSERT INTO users VALUES (1, 'alice', 30)");
    db_->Execute("INSERT INTO users VALUES (2, 'bob', 25)");

    server_ = std::make_unique<HttpServer>(db_.get(), /*port=*/0);
    server_thread_ = std::thread([this] { server_->Run(); });
  }

  void TearDown() override {
    server_->Stop();
    server_thread_.join();
    server_.reset();
    db_.reset();
    std::filesystem::remove(file_path_);
  }

  int Port() const { return server_->GetBoundPort(); }

  std::string file_path_;
  std::unique_ptr<Database> db_;
  std::unique_ptr<HttpServer> server_;
  std::thread server_thread_;
};

TEST_F(HttpServerTest, BoundPortIsAssignedForEphemeralRequest) { EXPECT_GT(Port(), 0); }

TEST_F(HttpServerTest, HealthCheckReturns200) {
  HttpResponse resp = SendRequest(Port(), "GET", "/health");
  EXPECT_EQ(resp.status, 200);
  EXPECT_EQ(resp.body, "ok");
}

TEST_F(HttpServerTest, SelectStarReturnsRowsAsJson) {
  HttpResponse resp = SendRequest(Port(), "GET", "/query?sql=SELECT%20*%20FROM%20users");
  EXPECT_EQ(resp.status, 200);
  EXPECT_NE(resp.body.find("\"columns\":[\"id\",\"name\",\"age\"]"), std::string::npos);
  EXPECT_NE(resp.body.find("[1,\"alice\",30]"), std::string::npos);
  EXPECT_NE(resp.body.find("[2,\"bob\",25]"), std::string::npos);
}

TEST_F(HttpServerTest, PointLookupReturnsSingleRow) {
  HttpResponse resp = SendRequest(Port(), "GET", "/query?sql=SELECT%20name%20FROM%20users%20WHERE%20id%20%3D%201");
  EXPECT_EQ(resp.status, 200);
  EXPECT_NE(resp.body.find("\"columns\":[\"name\"]"), std::string::npos);
  EXPECT_NE(resp.body.find("[\"alice\"]"), std::string::npos);
  EXPECT_EQ(resp.body.find("bob"), std::string::npos);
}

TEST_F(HttpServerTest, MissingSqlParamReturns400) {
  HttpResponse resp = SendRequest(Port(), "GET", "/query");
  EXPECT_EQ(resp.status, 400);
  EXPECT_NE(resp.body.find("\"error\""), std::string::npos);
}

TEST_F(HttpServerTest, NonSelectStatementRejected) {
  for (const std::string target : {
           "/query?sql=DELETE%20FROM%20users",
           "/query?sql=INSERT%20INTO%20users%20VALUES%20(3%2C%20'x'%2C%201)",
           "/query?sql=CREATE%20TABLE%20t2%20(id%20INTEGER)",
       }) {
    HttpResponse resp = SendRequest(Port(), "GET", target);
    EXPECT_EQ(resp.status, 400) << target;
    EXPECT_NE(resp.body.find("read-only"), std::string::npos) << target;
  }

  // None of the rejected statements actually ran.
  HttpResponse check = SendRequest(Port(), "GET", "/query?sql=SELECT%20*%20FROM%20users");
  EXPECT_NE(check.body.find("[1,\"alice\",30]"), std::string::npos);
  EXPECT_NE(check.body.find("[2,\"bob\",25]"), std::string::npos);
  EXPECT_EQ(check.body.find("[3,"), std::string::npos);
}

TEST_F(HttpServerTest, UnknownTableReturns400WithMessage) {
  HttpResponse resp = SendRequest(Port(), "GET", "/query?sql=SELECT%20*%20FROM%20nope");
  EXPECT_EQ(resp.status, 400);
  EXPECT_NE(resp.body.find("no such table"), std::string::npos);
}

TEST_F(HttpServerTest, UnknownRouteReturns404) {
  HttpResponse resp = SendRequest(Port(), "GET", "/bogus");
  EXPECT_EQ(resp.status, 404);
}

TEST_F(HttpServerTest, NonGetMethodReturns405) {
  HttpResponse resp = SendRequest(Port(), "POST", "/query?sql=SELECT%20*%20FROM%20users");
  EXPECT_EQ(resp.status, 405);
}

}  // namespace
}  // namespace dbengine
