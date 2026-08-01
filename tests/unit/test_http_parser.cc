/**
 * @file test_http_parser.cc
 * @brief Unit tests for the incremental HTTP/1.1 request parser.
 */

#include "catch_amalgamated.hpp"

#include "codicis/net/http_parser.h"
#include "codicis/util/buffer.h"

#include <string>
#include <string_view>

using namespace codicis;
using Progress = HttpRequestParser::Progress;

namespace {

/** @brief Append a string to a buffer. */
void Feed(Buffer& b, std::string_view s) { b.append(s); }

}  // namespace

TEST_CASE("Parses a simple GET request", "[http][parser]") {
  HttpRequestParser p;
  Buffer b;
  Feed(b, "GET /health?x=1 HTTP/1.1\r\nHost: a\r\n\r\n");
  REQUIRE(p.parse(b) == Progress::kComplete);
  REQUIRE(p.request().method == "GET");
  REQUIRE(p.request().target == "/health?x=1");
  REQUIRE(p.request().path == "/health");
  REQUIRE(p.request().query == "x=1");
  REQUIRE(p.request().http_minor == 1);
  REQUIRE(p.request().keep_alive);
  REQUIRE(p.request().header("host") != nullptr);
  REQUIRE(*p.request().header("HOST") == "a");
}

TEST_CASE("Waits for the full header block across partial feeds",
          "[http][parser]") {
  HttpRequestParser p;
  Buffer b;
  Feed(b, "GET / HTTP/1.1\r\nHo");
  REQUIRE(p.parse(b) == Progress::kIncomplete);
  Feed(b, "st: x\r\n\r");
  REQUIRE(p.parse(b) == Progress::kIncomplete);
  Feed(b, "\n");
  REQUIRE(p.parse(b) == Progress::kComplete);
  REQUIRE(p.request().path == "/");
}

TEST_CASE("Reads a Content-Length body split across feeds",
          "[http][parser]") {
  HttpRequestParser p;
  Buffer b;
  Feed(b, "POST /o HTTP/1.1\r\nContent-Length: 5\r\n\r\nhe");
  REQUIRE(p.parse(b) == Progress::kIncomplete);
  Feed(b, "llo");
  REQUIRE(p.parse(b) == Progress::kComplete);
  REQUIRE(p.request().body == "hello");
}

TEST_CASE("Decodes a chunked body", "[http][parser]") {
  HttpRequestParser p;
  Buffer b;
  Feed(b,
       "POST /o HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
       "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n");
  REQUIRE(p.parse(b) == Progress::kComplete);
  REQUIRE(p.request().body == "Wikipedia");
}

TEST_CASE("Decodes a chunked body arriving byte by byte", "[http][parser]") {
  HttpRequestParser p;
  Buffer b;
  const std::string msg =
      "POST /o HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
      "3\r\nabc\r\n0\r\n\r\n";
  Progress last = Progress::kIncomplete;
  for (char c : msg) {
    b.append(std::string_view(&c, 1));
    last = p.parse(b);
  }
  REQUIRE(last == Progress::kComplete);
  REQUIRE(p.request().body == "abc");
}

TEST_CASE("Handles pipelined requests with reset", "[http][parser]") {
  HttpRequestParser p;
  Buffer b;
  Feed(b, "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n");
  REQUIRE(p.parse(b) == Progress::kComplete);
  REQUIRE(p.request().path == "/a");
  p.reset();
  REQUIRE(p.parse(b) == Progress::kComplete);
  REQUIRE(p.request().path == "/b");
}

TEST_CASE("Resolves keep-alive from version and Connection header",
          "[http][parser]") {
  SECTION("HTTP/1.1 defaults keep-alive") {
    HttpRequestParser p;
    Buffer b;
    Feed(b, "GET / HTTP/1.1\r\n\r\n");
    REQUIRE(p.parse(b) == Progress::kComplete);
    REQUIRE(p.request().keep_alive);
  }
  SECTION("HTTP/1.1 with Connection: close") {
    HttpRequestParser p;
    Buffer b;
    Feed(b, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
    REQUIRE(p.parse(b) == Progress::kComplete);
    REQUIRE_FALSE(p.request().keep_alive);
  }
  SECTION("HTTP/1.0 defaults to close") {
    HttpRequestParser p;
    Buffer b;
    Feed(b, "GET / HTTP/1.0\r\n\r\n");
    REQUIRE(p.parse(b) == Progress::kComplete);
    REQUIRE_FALSE(p.request().keep_alive);
  }
  SECTION("HTTP/1.0 with keep-alive") {
    HttpRequestParser p;
    Buffer b;
    Feed(b, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    REQUIRE(p.parse(b) == Progress::kComplete);
    REQUIRE(p.request().keep_alive);
  }
}

TEST_CASE("Rejects malformed input", "[http][parser]") {
  SECTION("bad request line") {
    HttpRequestParser p;
    Buffer b;
    Feed(b, "GARBAGE\r\n\r\n");
    REQUIRE(p.parse(b) == Progress::kError);
  }
  SECTION("bad version") {
    HttpRequestParser p;
    Buffer b;
    Feed(b, "GET / HTTP/2.0\r\n\r\n");
    REQUIRE(p.parse(b) == Progress::kError);
  }
  SECTION("header block too large") {
    HttpRequestParser p(/*max_header_bytes=*/16);
    Buffer b;
    Feed(b, "GET /verylongpath/that/exceeds HTTP/1.1\r\n\r\n");
    REQUIRE(p.parse(b) == Progress::kError);
  }
  SECTION("body exceeds limit") {
    HttpRequestParser p(/*max_header_bytes=*/1024, /*max_body_bytes=*/2);
    Buffer b;
    Feed(b, "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello");
    REQUIRE(p.parse(b) == Progress::kError);
  }
}
