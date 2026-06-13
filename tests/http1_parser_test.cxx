#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.http1_parser;

using conflux::http::ParserLimits;

TEST_CASE(
	"http1_parser: valid GET request parses correctly") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET /path HTTP/1.1\r\nHost: localhost\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.method == "GET");
	REQUIRE(out.target == "/path");
	REQUIRE(out.version == "HTTP/1.1");
	REQUIRE(out.headers.size() == 1);
	REQUIRE(out.headers[0].first == "Host");
	REQUIRE(out.headers[0].second == "localhost");
}

TEST_CASE(
	"http1_parser: incomplete request returns Incomplete") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET /path HTTP/1.1\r\nHost: localhost\r\n", limits, out);
	REQUIRE(status == ParseStatus::Incomplete);
}

TEST_CASE(
	"http1_parser: missing HTTP version returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET /path NOTHTTP\r\nHost: x\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: request with no headers parses correctly") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.method == "GET");
	REQUIRE(out.target == "/");
	REQUIRE(out.headers.empty());
}

TEST_CASE(
	"http1_parser: incomplete request line over limit returns UriTooLong") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_request_line_size = 12;
	ParsedRequest out;
	auto status = parse_request("GET /still-growing", limits, out);
	REQUIRE(status == ParseStatus::UriTooLong);
}

TEST_CASE(
	"http1_parser: URI too long returns UriTooLong") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_request_line_size = 10;
	ParsedRequest out;
	auto status = parse_request("GET /very-long-path-here HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::UriTooLong);
}

TEST_CASE(
	"http1_parser: header with null std::byte returns BadRequest") {
	using namespace conflux::http1;
	using namespace std::string_literals;
	ParserLimits const limits{};
	ParsedRequest out;
	std::string raw = "GET / HTTP/1.1\r\nX-Bad: val\x00ue\r\n\r\n"s;
	auto status = parse_request(raw, limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: request target with control byte returns BadRequest") {
	using namespace conflux::http1;
	using namespace std::string_literals;
	ParserLimits const limits{};

	for (std::string const &raw: {
			 "GET /slow/\nforged HTTP/1.1\r\n\r\n"s,
			 "GET /slow/\rforged HTTP/1.1\r\n\r\n"s,
			 "GET /slow/\x1b[31m HTTP/1.1\r\n\r\n"s,
			 "GET /slow/\x7f HTTP/1.1\r\n\r\n"s,
		 }) {
		ParsedRequest out;
		auto status = parse_request(raw, limits, out);
		REQUIRE(status == ParseStatus::BadRequest);
	}
}

TEST_CASE(
	"http1_parser: incomplete header line over limit returns HeaderLineTooLarge") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_header_line_size = 8;
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nX-Long: still-growing", limits, out);
	REQUIRE(status == ParseStatus::HeaderLineTooLarge);
}

TEST_CASE(
	"http1_parser: incomplete header count over limit returns TooManyHeaders") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_headers = 2;
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n", limits, out);
	REQUIRE(status == ParseStatus::TooManyHeaders);
}

TEST_CASE(
	"http1_parser: too many headers returns TooManyHeaders") {
	using namespace conflux::http1;
	ParserLimits limits{};
	limits.max_headers = 2;
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::TooManyHeaders);
}

TEST_CASE(
	"http1_parser: header value leading/trailing whitespace is stripped") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nX-Foo:   bar  \r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.headers.size() == 1);
	REQUIRE(out.headers[0].second == "bar");
}

TEST_CASE(
	"http1_parser: HTTP/1.0 version is accepted") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.0\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::Ok);
	REQUIRE(out.version == "HTTP/1.0");
}

TEST_CASE(
	"http1_parser: empty method returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request(" /path HTTP/1.1\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: invalid tchar in header name returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nX Bad: value\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: folded header line returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\nX-Foo: bar\r\n  continuation\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}

TEST_CASE(
	"http1_parser: header with empty name (bare colon) returns BadRequest") {
	using namespace conflux::http1;
	ParserLimits const limits{};
	ParsedRequest out;
	auto status = parse_request("GET / HTTP/1.1\r\n: value\r\n\r\n", limits, out);
	REQUIRE(status == ParseStatus::BadRequest);
}
