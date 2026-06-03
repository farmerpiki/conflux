#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.json;
import conflux.net.config;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::uint16_t g_parser_port = 0;

void ensure_parser_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.get("/api/ping", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text(R"({"status":"ok"})");
		});
		router.post("/api/echo-body", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.body});
		});
		router.get("/api/echo-header", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["x-test-header"]});
		});
		g_parser_port = start_mw_server(mw_config(), std::move(router));
	});
}

ReadUntilCloseResult send_raw_bytes_result_on(
	std::uint16_t port,
	std::string_view raw) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket failed"};
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"connect failed"};
	}
	timeval tv{.tv_sec = 3, .tv_usec = 0};
	::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
	auto response = read_until_close_with_state(fd);
	::close(fd);
	return response;
}

ReadUntilCloseResult send_raw_bytes_result(
	std::string_view raw) {
	ensure_parser_server();
	return send_raw_bytes_result_on(g_parser_port, raw);
}

std::string send_raw_bytes(
	std::string_view raw) {
	return send_raw_bytes_result(raw).bytes;
}

std::string send_raw_bytes_on(
	std::uint16_t port,
	std::string_view raw) {
	return send_raw_bytes_result_on(port, raw).bytes;
}

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

void check_json_string_at(
	conflux::json::Document const &doc,
	std::string_view pointer,
	std::string_view expected) {
	auto node = doc.root().at_pointer(pointer);
	REQUIRE(node.has_value());
	auto value = node->as_string();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}

void check_parser_problem_code(
	std::string_view response,
	std::string_view code) {
	CHECK(response.find("application/problem+json") != std::string_view::npos);
	auto doc = conflux::json::parse_copy(extract_body(response));
	REQUIRE(doc.has_value());
	check_json_string_at(*doc, "/code", code);
}

bool server_closed_after(
	std::string_view path,
	std::string_view extra_headers) {
	ensure_parser_server();

	LocalTcpClient client{g_parser_port};
	auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\n{}\r\n", path, extra_headers);
	auto sent = client.send(req);
	REQUIRE(sent > 0);
	auto response = client.read_one_response();
	REQUIRE(response.starts_with("HTTP/1.1 200"));

	char probe{};
	auto n = client.recv(&probe, 1);
	return n == 0;
}

} // namespace

TEST_CASE(
	"parser: request line exceeding 8 KiB returns 414") {
	std::string path = "/";
	path.append(9000, 'a');
	auto req = std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 414"));
}

TEST_CASE(
	"parser: invalid method token returns 400") {
	auto resp = send_raw_bytes("GE<T /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: empty request target returns 400") {
	auto resp = send_raw_bytes("GET  HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: single header line exceeding 8 KiB returns 431") {
	std::string header_value(9000, 'v');
	auto req = std::format(
		"GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-Big: {}\r\nConnection: close\r\n\r\n",
		header_value);
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 431"));
}

TEST_CASE(
	"parser: more than 100 headers returns 431") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
	for (int i = 0; i < 120; ++i) {
		req += std::format("X-H-{}: v\r\n", i);
	}
	req += "\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 431"));
}

TEST_CASE(
	"parser: obs-fold line returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-A: one\r\n two\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: NUL std::byte in header returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nX-Bad: a";
	req.push_back('\0');
	req += "b\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: header missing colon returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost: localhost\r\nNoColonHere\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: field-name with space before colon returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nHost : localhost\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: header with no space after colon is accepted") {
	std::string req =
		"GET /api/echo-header HTTP/1.1\r\nHost: localhost\r\nX-Test-Header:no-space\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	REQUIRE(resp.find("no-space") != std::string::npos);
}

TEST_CASE(
	"parser: header value is trimmed of leading and trailing OWS") {
	std::string req =
		"GET /api/echo-header HTTP/1.1\r\nHost: localhost\r\nX-Test-Header:   spaced   \r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	REQUIRE(extract_body(resp) == "spaced");
}

TEST_CASE(
	"parser: malformed Content-Length returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5abc\r\nConnection: close\r\n\r\nhello";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: duplicate Content-Length returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 5\r\n"
		"Connection: close\r\n\r\nhello";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: missing Host in HTTP/1.1 returns 400") {
	std::string req = "GET /api/ping HTTP/1.1\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: duplicate Host in HTTP/1.1 returns 400") {
	std::string req =
		"GET /api/ping HTTP/1.1\r\nHost: localhost\r\nHost: attacker.example\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: Content-Length with Transfer-Encoding returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nTransfer-Encoding: "
		"chunked\r\nConnection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: Content-Length plus Transfer-Encoding smuggling attempt closes before pipelined request") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nTransfer-Encoding: "
		"chunked\r\n\r\n0\r\n\r\n"
		"GET /api/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	auto resp = send_raw_bytes_result(req);
	REQUIRE(resp.closed());
	REQUIRE(resp.bytes.starts_with("HTTP/1.1 400"));
	REQUIRE(resp.bytes.find("HTTP/1.1 200") == std::string::npos);
	REQUIRE(resp.bytes.find(R"({"status":"ok"})") == std::string::npos);
}

TEST_CASE(
	"parser: unsupported Transfer-Encoding returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: gzip, chunked\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: Transfer-Encoding after chunked returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked, gzip\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: duplicate Transfer-Encoding headers return 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
		"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: empty Transfer-Encoding token returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked,\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: uppercase chunked Transfer-Encoding is accepted") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: CHUNKED\r\n"
		"Connection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
}

TEST_CASE(
	"parser: Connection close token closes persistent connection") {
	REQUIRE(server_closed_after("/api/ping", "Connection: keep-alive, close\r\n"));
}

TEST_CASE(
	"parser: chunked transfer with chunk-count overflow returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
	for (int i = 0; i < 200000; ++i) {
		req += "1\r\nx\r\n";
	}
	req += "0\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: chunked transfer with oversized trailer returns 400") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
		"0\r\nX-Trailer: ";
	req.append(9000, 'x');
	req += "\r\n\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 400"));
}

TEST_CASE(
	"parser: chunked transfer with huge declared chunk returns 413") {
	std::string req =
		"POST /api/echo-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
		"ffffffffffffffff\r\n";
	auto resp = send_raw_bytes(req);
	REQUIRE(resp.starts_with("HTTP/1.1 413"));
}

TEST_CASE(
	"parser: rejection metrics count classified HTTP/1 rejects") {
	conflux::http::Router router;
	router.post("/echo", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.body});
	});
	auto cfg = mw_config();
	cfg.max_body_size = 4;
	ScopedTestServer srv{cfg, std::move(router)};
	auto const port = srv.port();

	auto malformed_cl = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5abc\r\nConnection: close\r\n\r\nhello");
	REQUIRE(malformed_cl.starts_with("HTTP/1.1 400"));
	check_parser_problem_code(malformed_cl, "malformed_content_length");

	auto duplicate_cl = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\nContent-Length: 2\r\n"
		"Connection: close\r\n\r\nhi");
	REQUIRE(duplicate_cl.starts_with("HTTP/1.1 400"));
	check_parser_problem_code(duplicate_cl, "duplicate_content_length");

	auto cl_te = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n"
		"Connection: close\r\n\r\n0\r\n\r\n");
	REQUIRE(cl_te.starts_with("HTTP/1.1 400"));
	check_parser_problem_code(cl_te, "content_length_with_transfer_encoding");

	std::string long_path = "/";
	long_path.append(9000, 'a');
	auto request_line = send_raw_bytes_on(
		port,
		std::format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", long_path));
	REQUIRE(request_line.starts_with("HTTP/1.1 414"));
	check_parser_problem_code(request_line, "request_line_too_large");

	std::string header_line_value(9000, 'v');
	auto header_line = send_raw_bytes_on(
		port,
		std::format(
			"GET /echo HTTP/1.1\r\nHost: localhost\r\nX-Big: {}\r\nConnection: close\r\n\r\n",
			header_line_value));
	REQUIRE(header_line.starts_with("HTTP/1.1 431"));
	check_parser_problem_code(header_line, "header_line_too_large");

	std::string header_block_req = "GET /echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
	for (int i = 0; i < 80; ++i) {
		header_block_req += std::format("X-Block-{}: {}\r\n", i, std::string(900, 'v'));
	}
	header_block_req += "\r\n";
	auto header_block = send_raw_bytes_on(port, header_block_req);
	REQUIRE(header_block.starts_with("HTTP/1.1 431"));
	check_parser_problem_code(header_block, "header_block_too_large");

	std::string too_many_headers_req = "GET /echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
	for (int i = 0; i < 120; ++i) {
		too_many_headers_req += std::format("X-Count-{}: v\r\n", i);
	}
	too_many_headers_req += "\r\n";
	auto too_many_headers = send_raw_bytes_on(port, too_many_headers_req);
	REQUIRE(too_many_headers.starts_with("HTTP/1.1 431"));
	check_parser_problem_code(too_many_headers, "too_many_headers");

	auto too_large_body = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
	REQUIRE(too_large_body.starts_with("HTTP/1.1 413"));
	check_parser_problem_code(too_large_body, "body_too_large");

	auto invalid_chunk = send_raw_bytes_on(
		port,
		"POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
		"z\r\nx\r\n");
	REQUIRE(invalid_chunk.starts_with("HTTP/1.1 400"));
	check_parser_problem_code(invalid_chunk, "invalid_chunk");

	srv.stop();
	auto const metrics = srv.metrics();
	CHECK(metrics.rejections.malformed_content_length == 1);
	CHECK(metrics.rejections.duplicate_content_length == 1);
	CHECK(metrics.rejections.content_length_with_transfer_encoding == 1);
	CHECK(metrics.rejections.request_line_too_large == 1);
	CHECK(metrics.rejections.header_line_too_large == 1);
	CHECK(metrics.rejections.header_block_too_large == 1);
	CHECK(metrics.rejections.too_many_headers == 1);
	CHECK(metrics.rejections.body_too_large == 1);
	CHECK(metrics.rejections.invalid_chunk == 1);
}
