// Plain TU — not a module unit. std::thread lambda → module TU-local rule.
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.types;
import conflux.net.http;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

S read_exactly(
	int fd,
	SZ expected,
	chrono::milliseconds budget = chrono::seconds{10}) {
	S out;
	out.resize(expected);
	SZ off = 0;
	auto const deadline = chrono::steady_clock::now() + budget;
	while (off < expected) {
		if (chrono::steady_clock::now() > deadline) {
			break;
		}
		ssize_t const n = ::recv(fd, out.data() + off, expected - off, 0);
		if (n <= 0) {
			break;
		}
		off += static_cast<SZ>(n);
	}
	out.resize(off);
	return out;
}

P<S, S> send_get_split_body(
	u16 port,
	SV path,
	SZ expected_body) {
	int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(fd >= 0);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
	REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);

	auto const req = format("GET {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
	::send(fd, req.data(), req.size(), 0);

	S buf;
	A<char, 64UL * 1024> tmp{};
	while (buf.find("\r\n\r\n") == S::npos) {
		ssize_t const n = ::recv(fd, tmp.data(), tmp.size(), 0);
		if (n <= 0) {
			::close(fd);
			return {move(buf), {}};
		}
		buf.append(tmp.data(), static_cast<SZ>(n));
	}
	auto const hdr_end = buf.find("\r\n\r\n");
	S headers = buf.substr(0, hdr_end + 4);
	S body = buf.substr(hdr_end + 4);
	if (body.size() < expected_body) {
		body += read_exactly(fd, expected_body - body.size());
	}
	::close(fd);
	return {move(headers), move(body)};
}

struct StaticDir {
	S path;
	explicit StaticDir(
		char const *pattern = "/tmp/conflux_file_io_http_XXXXXX") {
		path = pattern;
		REQUIRE(::mkdtemp(path.data()) != nullptr);
	}
	~StaticDir() {
		for (auto &ent: fs::directory_iterator{path}) {
			(void)ent;
		}
		EC ec;
		fs::remove_all(path, ec);
	}
	StaticDir(StaticDir const &) = delete;
	StaticDir &operator =(StaticDir const &) = delete;

	void write(
		SV name,
		SV content) const {
		auto full = path + "/" + S{name};
		int const fd = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		REQUIRE(fd >= 0);
		ssize_t off = 0;
		while (off < static_cast<ssize_t>(content.size())) {
			ssize_t const n = ::write(fd, content.data() + off, content.size() - static_cast<SZ>(off));
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
	S const content(2UL * 1024 * 1024, 'A'); // 2 MiB → spans many splice chunks
	dir.write("big.bin", content);
	dir.write("small.txt", "tiny");

	Config cfg = mw_config();
	cfg.fixed_buffer_slabs = 4;
	cfg.fixed_buffer_bytes = 64UL * 1024;
	cfg.splice_pipe_pairs = 4;

	Router router;
	router.serve_static("/static", dir.path);

	ScopedTestServer const srv{cfg, move(router)};

	SECTION("2 MiB file GET over plain HTTP") {
		auto [headers, body] = send_get_split_body(srv.port(), "/static/big.bin", content.size());
		REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(headers.find(format("Content-Length: {}\r\n", content.size())) != S::npos);
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
		auto const hdr_req = format(
			"GET /static/big.bin HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nRange: bytes=100-199\r\n\r\n");
		int const fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(srv.port());
		::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
		REQUIRE(::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		::send(fd, hdr_req.data(), hdr_req.size(), 0);

		S resp;
		A<char, 8192> tmp{};
		while (resp.find("\r\n\r\n") == S::npos) {
			ssize_t const n = ::recv(fd, tmp.data(), tmp.size(), 0);
			if (n <= 0) {
				break;
			}
			resp.append(tmp.data(), static_cast<SZ>(n));
		}
		auto const hdr_end = resp.find("\r\n\r\n");
		S body = resp.substr(hdr_end + 4);
		body += read_exactly(fd, 100 - body.size());
		::close(fd);

		CHECK(resp.starts_with("HTTP/1.1 206"));
		CHECK(body.size() == 100);
		CHECK(body == S(100, 'A'));
	}
}

TEST_CASE(
	"file_io http e2e: disabled pools fall back to mmap static serving",
	"[file_io][http][e2e]") {
	StaticDir const dir{"/tmp/conflux_file_io_http_fallback_XXXXXX"};
	S const content(256UL * 1024, 'F');
	dir.write("fallback.bin", content);

	Config cfg = mw_config();
	cfg.fixed_buffer_slabs = 4;
	cfg.fixed_buffer_bytes = 64UL * 1024;
	cfg.splice_pipe_pairs = 0;

	Router router;
	router.serve_static("/static", dir.path);

	ScopedTestServer const srv{cfg, move(router)};
	auto [headers, body] = send_get_split_body(srv.port(), "/static/fallback.bin", content.size());
	REQUIRE(headers.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(headers.find(format("Content-Length: {}\r\n", content.size())) != S::npos);
	CHECK(body == content);
}
