// Plain TU — not a module unit. std::thread lambda → module TU-local rule.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http.static_files;
import conflux.net.http_server;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;
using conflux::http::Config;
namespace {

std::string read_exactly(
	int fd,
	std::size_t expected,
	std::chrono::milliseconds budget = std::chrono::seconds{10}) {
	std::string out;
	out.resize(expected);
	std::size_t off = 0;
	auto const deadline = std::chrono::steady_clock::now() + budget;
	while (off < expected) {
		if (std::chrono::steady_clock::now() > deadline) {
			break;
		}
		ssize_t const n = ::recv(fd, out.data() + off, expected - off, 0);
		if (n <= 0) {
			break;
		}
		off += static_cast<std::size_t>(n);
	}
	out.resize(off);
	return out;
}
std::pair<std::string, std::string> send_request_split_body(
	std::uint16_t port,
	std::string_view method,
	std::string_view path,
	std::string_view extra_headers,
	std::size_t expected_body) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	auto const req =
		std::format("{} {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n{}\r\n", method, path, extra_headers);
	::send(fd, req.data(), req.size(), 0);

	std::string buf;
	std::array<char, 64UL * 1024> tmp{};
	while (buf.find("\r\n\r\n") == std::string::npos) {
		ssize_t const n = ::recv(fd, tmp.data(), tmp.size(), 0);
		if (n <= 0) {
			::close(fd);
			return {std::move(buf), {}};
		}
		buf.append(tmp.data(), static_cast<std::size_t>(n));
	}
	auto const hdr_end = buf.find("\r\n\r\n");
	std::string headers = buf.substr(0, hdr_end + 4);
	std::string body = buf.substr(hdr_end + 4);
	if (body.size() < expected_body) {
		body += read_exactly(fd, expected_body - body.size());
	}
	::close(fd);
	return {std::move(headers), std::move(body)};
}
std::pair<std::string, std::string> send_get_split_body(
	std::uint16_t port,
	std::string_view path,
	std::size_t expected_body) {
	return send_request_split_body(port, "GET", path, "", expected_body);
}
std::pair<std::string, std::string> read_response_split_body(
	int fd,
	std::string &pending) {
	std::array<char, 64UL * 1024> tmp{};
	while (pending.find("\r\n\r\n") == std::string::npos) {
		ssize_t const n = ::recv(fd, tmp.data(), tmp.size(), 0);
		if (n <= 0) {
			return {std::move(pending), {}};
		}
		pending.append(tmp.data(), static_cast<std::size_t>(n));
	}
	auto const hdr_end = pending.find("\r\n\r\n");
	std::string headers = pending.substr(0, hdr_end + 4);
	pending.erase(0, hdr_end + 4);
	std::size_t expected_body = 0;
	std::string_view const cl_name = "Content-Length: ";
	if (auto const cl_pos = headers.find(cl_name); cl_pos != std::string::npos) {
		auto const cl_start = cl_pos + cl_name.size();
		auto const cl_end = headers.find("\r\n", cl_start);
		auto const value = std::string_view{headers}.substr(cl_start, cl_end - cl_start);
		auto const _ = std::from_chars(value.data(), value.data() + value.size(), expected_body);
	}
	while (pending.size() < expected_body) {
		ssize_t const n = ::recv(fd, tmp.data(), tmp.size(), 0);
		if (n <= 0) {
			break;
		}
		pending.append(tmp.data(), static_cast<std::size_t>(n));
	}
	std::string body = pending.substr(0, expected_body);
	pending.erase(0, std::min(pending.size(), expected_body));
	return {std::move(headers), std::move(body)};
}
std::optional<std::string> header_value(
	std::string_view headers,
	std::string_view name) {
	std::string const needle = std::format("{}: ", name);
	auto const pos = headers.find(needle);
	if (pos == std::string_view::npos) {
		return std::nullopt;
	}
	auto const value_start = pos + needle.size();
	auto const value_end = headers.find("\r\n", value_start);
	if (value_end == std::string_view::npos) {
		return std::nullopt;
	}
	return std::string{headers.substr(value_start, value_end - value_start)};
}
struct StaticDir {
	std::string path;
	explicit StaticDir(
		char const *pattern = "/tmp/conflux_file_io_http_XXXXXX") {
		path = pattern;
		REQUIRE(::mkdtemp(path.data()) != nullptr);
	}
	~StaticDir() {
		for (auto const &ent: std::ranges::subrange{std::filesystem::directory_iterator{path}, std::default_sentinel}) {
			(void)ent;
		}
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
	StaticDir(StaticDir const &) = delete;
	StaticDir &operator =(StaticDir const &) = delete;
	void write(
		std::string_view name,
		std::string_view content) const {
		auto full = path + "/" + std::string{name};
		int const fd = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(fd >= 0);
		ssize_t off = 0;
		while (off < static_cast<ssize_t>(content.size())) {
			ssize_t const n = ::write(fd, content.data() + off, content.size() - static_cast<std::size_t>(off));
			REQUIRE(n > 0);
			off += n;
		}
		::close(fd);
	}
};

} // namespace
TEST_CASE(
	"file_io http e2e: large file delivered via uring splice path",
	"[file_io][http][e2e]") {
	StaticDir const dir;
	std::string const content(2UL * 1024 * 1024, 'A'); // 2 MiB → spans many splice chunks
	dir.write("big.bin", content);
	dir.write("small.txt", "tiny");

	Config cfg = mw_config();
	cfg.fixed_buffer_slabs = 4;
	cfg.fixed_buffer_bytes = 64UL * 1024;
	cfg.splice_pipe_pairs = 4;

	conflux::http::Router router;
	router.serve_static("/static", dir.path);

	ScopedTestServer const srv{cfg, std::move(router)};

	SECTION("2 MiB file GET over plain HTTP") {
		auto [headers, body] = send_get_split_body(srv.port(), "/static/big.bin", content.size());
		REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(headers.find(std::format("Content-Length: {}\r\n", content.size())) != std::string::npos);
		REQUIRE(body.size() == content.size());
		CHECK(body == content);
	}

	SECTION("small file unchanged") {
		auto [headers, body] = send_get_split_body(srv.port(), "/static/small.txt", 4);
		REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));
		CHECK(body == "tiny");
	}

	SECTION("404 for missing file") {
		auto [headers, body] = send_get_split_body(srv.port(), "/static/nope.bin", 0);
		REQUIRE(headers.starts_with("HTTP/1.1 404 Not Found"));
	}

	SECTION("range request returns 206") {
		auto const hdr_req = std::format(
			"GET /static/big.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nRange: bytes=100-199\r\n\r\n");
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(srv.port());
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		::send(fd, hdr_req.data(), hdr_req.size(), 0);

		std::string resp;
		std::array<char, 8192> tmp{};
		while (resp.find("\r\n\r\n") == std::string::npos) {
			ssize_t const n = ::recv(fd, tmp.data(), tmp.size(), 0);
			if (n <= 0) {
				break;
			}
			resp.append(tmp.data(), static_cast<std::size_t>(n));
		}
		auto const hdr_end = resp.find("\r\n\r\n");
		std::string body = resp.substr(hdr_end + 4);
		body += read_exactly(fd, 100 - body.size());
		::close(fd);

		CHECK(resp.starts_with("HTTP/1.1 206"));
		CHECK(body.size() == 100);
		CHECK(body == std::string(100, 'A'));
	}

	SECTION("large static response preserves pipelined follow-up request") {
		conflux::http::Router pipelined_router;
		pipelined_router.serve_static("/static", dir.path);
		pipelined_router.get("/after", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("after-static");
		});
		ScopedTestServer const pipelined_srv{cfg, std::move(pipelined_router)};

		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		REQUIRE(fd >= 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(pipelined_srv.port());
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		std::string const req =
			"GET /static/big.bin HTTP/1.1\r\nHost: localhost\r\n\r\n"
			"GET /after HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
		REQUIRE(::send(fd, req.data(), req.size(), 0) == static_cast<ssize_t>(req.size()));

		std::string pending;
		auto [first_headers, first_body] = read_response_split_body(fd, pending);
		auto [second_headers, second_body] = read_response_split_body(fd, pending);
		::close(fd);

		REQUIRE(first_headers.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(first_headers.find(std::format("Content-Length: {}\r\n", content.size())) != std::string::npos);
		REQUIRE(first_body == content);
		REQUIRE(second_headers.starts_with("HTTP/1.1 200 OK"));
		CHECK(second_body == "after-static");
	}
}
TEST_CASE(
	"file_io http e2e: disabled pools fall back to mmap static serving",
	"[file_io][http][e2e]") {
	StaticDir const dir{"/tmp/conflux_file_io_http_fallback_XXXXXX"};
	std::string const content(256UL * 1024, 'F');
	dir.write("fallback.bin", content);

	Config cfg = mw_config();
	cfg.fixed_buffer_slabs = 4;
	cfg.fixed_buffer_bytes = 64UL * 1024;
	cfg.splice_pipe_pairs = 0;

	conflux::http::Router router;
	router.serve_static("/static", dir.path);

	ScopedTestServer const srv{cfg, std::move(router)};
	auto [headers, body] = send_get_split_body(srv.port(), "/static/fallback.bin", content.size());
	REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(headers.find(std::format("Content-Length: {}\r\n", content.size())) != std::string::npos);
	CHECK(body == content);
}

TEST_CASE(
	"file_io http e2e: static validators and HEAD semantics",
	"[file_io][http][e2e][static]") {
	StaticDir const dir{"/tmp/conflux_file_io_http_conditional_XXXXXX"};
	std::string const content = "hello-static-cache";
	dir.write("asset.txt", content);

	Config cfg = mw_config();
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;

	conflux::http::Router router;
	router.serve_static("/static", dir.path);

	ScopedTestServer const srv{cfg, std::move(router)};
	auto [get_headers, get_body] = send_get_split_body(srv.port(), "/static/asset.txt", content.size());
	REQUIRE(get_headers.starts_with("HTTP/1.1 200 OK"));
	CHECK(get_body == content);

	auto const etag = header_value(get_headers, "ETag");
	auto const last_modified = header_value(get_headers, "Last-Modified");
	REQUIRE(etag.has_value());
	REQUIRE(last_modified.has_value());
	CHECK(header_value(get_headers, "Accept-Ranges") == std::optional<std::string>{"bytes"});

	auto [head_headers, head_body] = send_request_split_body(srv.port(), "HEAD", "/static/asset.txt", "", 0);
	REQUIRE(head_headers.starts_with("HTTP/1.1 200 OK"));
	CHECK(head_body.empty());
	CHECK(head_headers.find(std::format("Content-Length: {}\r\n", content.size())) != std::string::npos);
	CHECK(header_value(head_headers, "ETag") == etag);
	CHECK(header_value(head_headers, "Last-Modified") == last_modified);

	auto [etag_headers, etag_body] =
		send_request_split_body(srv.port(), "GET", "/static/asset.txt", std::format("If-None-Match: {}\r\n", *etag), 0);
	CHECK(etag_headers.starts_with("HTTP/1.1 304 Not Modified"));
	CHECK(etag_body.empty());

	auto [mtime_headers, mtime_body] = send_request_split_body(
		srv.port(),
		"GET",
		"/static/asset.txt",
		std::format("If-Modified-Since: {}\r\n", *last_modified),
		0);
	CHECK(mtime_headers.starts_with("HTTP/1.1 304 Not Modified"));
	CHECK(mtime_body.empty());
}

TEST_CASE(
	"file_io http e2e: static range edge semantics",
	"[file_io][http][e2e][static]") {
	StaticDir const dir{"/tmp/conflux_file_io_http_range_XXXXXX"};
	std::string const content = "0123456789abcdef";
	dir.write("bytes.txt", content);

	Config cfg = mw_config();
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;

	conflux::http::Router router;
	router.serve_static("/static", dir.path);

	ScopedTestServer const srv{cfg, std::move(router)};
	SECTION("suffix ranges return the requested tail") {
		auto [headers, body] =
			send_request_split_body(srv.port(), "GET", "/static/bytes.txt", "Range: bytes=-4\r\n", 4);
		REQUIRE(headers.starts_with("HTTP/1.1 206 Partial Content"));
		CHECK(headers.find("Content-Range: bytes 12-15/16\r\n") != std::string::npos);
		CHECK(body == "cdef");
	}

	SECTION("unsatisfiable range returns 416 with total size") {
		auto [headers, body] =
			send_request_split_body(srv.port(), "GET", "/static/bytes.txt", "Range: bytes=64-127\r\n", 0);
		CHECK(headers.starts_with("HTTP/1.1 416 Range Not Satisfiable"));
		CHECK(headers.find("Content-Range: bytes */16\r\n") != std::string::npos);
		CHECK(body.empty());
	}

	SECTION("unsupported multi-range is ignored instead of mis-serving partial data") {
		auto [headers, body] =
			send_request_split_body(srv.port(), "GET", "/static/bytes.txt", "Range: bytes=0-1,4-5\r\n", content.size());
		REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));
		CHECK(headers.find("Content-Range:") == std::string::npos);
		CHECK(body == content);
	}

	SECTION("malformed range is ignored instead of becoming a partial response") {
		auto [headers, body] =
			send_request_split_body(srv.port(), "GET", "/static/bytes.txt", "Range: bytes=abc-def\r\n", content.size());
		REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));
		CHECK(headers.find("Content-Range:") == std::string::npos);
		CHECK(body == content);
	}
}

TEST_CASE(
	"file_io http e2e: static symlink target is not served",
	"[file_io][http][e2e][static][security]") {
	StaticDir const dir{"/tmp/conflux_file_io_http_symlink_XXXXXX"};
	dir.write("real.txt", "secret-through-symlink");
	auto const link_path = dir.path + "/link.txt";
	REQUIRE(::symlink("real.txt", link_path.c_str()) == 0);

	Config cfg = mw_config();
	cfg.fixed_buffer_slabs = 0;
	cfg.splice_pipe_pairs = 0;

	conflux::http::Router router;
	router.serve_static("/static", dir.path);

	ScopedTestServer const srv{cfg, std::move(router)};
	auto [headers, body] = send_get_split_body(srv.port(), "/static/link.txt", 0);
	CHECK(headers.starts_with("HTTP/1.1 404 Not Found"));
	CHECK(body.find("secret-through-symlink") == std::string::npos);

	auto [real_headers, real_body] = send_get_split_body(srv.port(), "/static/real.txt", 22);
	REQUIRE(real_headers.starts_with("HTTP/1.1 200 OK"));
	CHECK(real_body == "secret-through-symlink");
}
