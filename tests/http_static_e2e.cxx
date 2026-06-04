#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.net.config;
import conflux.net.router;
import conflux.net.http.static_core;
import conflux.tests.support;
import conflux.work;

using namespace conflux::tests;

namespace {

std::string_view response_body(
	std::string_view response) {
	auto body_start = response.find("\r\n\r\n");
	REQUIRE(body_start != std::string_view::npos);
	return response.substr(body_start + 4);
}

std::string extract_header(
	std::string_view resp,
	std::string_view name) {
	auto needle = std::string{name} + ": ";
	auto pos = resp.find(needle);
	if (pos == std::string_view::npos) {
		return {};
	}
	pos += needle.size();
	auto end = resp.find("\r\n", pos);
	if (end == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos, end - pos)};
}

void write_file(
	std::string_view dir,
	std::string_view name,
	std::string_view content) {
	auto path = std::string{dir} + "/" + std::string{name};
	int const wfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	REQUIRE(wfd >= 0);
	REQUIRE(::write(wfd, content.data(), content.size()) == static_cast<ssize_t>(content.size()));
	::close(wfd);
}

std::string raw_request_on(
	std::uint16_t port,
	std::string_view method,
	std::string_view path,
	std::string_view body = "",
	std::string_view extra_headers = "") {
	LocalTcpClient const client{port};
	auto request = std::format(
		"{} {} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n{}Connection: close\r\n\r\n{}",
		method,
		path,
		body.size(),
		extra_headers,
		body);
	auto const sent = client.send(request);
	REQUIRE(sent >= 0);
	REQUIRE(static_cast<std::size_t>(sent) == request.size());
	return client.read_one_response();
}

std::string get_with_extra_header(
	std::uint16_t port,
	std::string_view path,
	std::string_view extra_header) {
	return http_get_on(port, path, std::format("{}Connection: close\r\n", extra_header));
}

std::string head_static_on(
	std::uint16_t port,
	std::string_view path) {
	LocalTcpClient const client{port};
	auto request = std::format("HEAD {} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
	auto const sent = client.send(request);
	REQUIRE(sent >= 0);
	REQUIRE(static_cast<std::size_t>(sent) == request.size());
	return client.read_one_response();
}

} // namespace

TEST_CASE(
	"static file serving: percent-encoded filename in URL is decoded and served") {
	char tmpdir[] = "/tmp/conflux_enc_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "hello world.txt", "space file");

	conflux::http::Router router;
	router.serve_static("/s", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/hello%20world.txt");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "space file");

	srv.stop();
	::unlink((std::string{tmpdir} + "/hello world.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: If-Modified-Since matching the file mtime returns 304") {
	char tmpdir[] = "/tmp/conflux_ims_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "test.txt", "hello");

	conflux::http::Router router;
	router.serve_static("/s", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp1 = conflux::tests::http_get_on(srv.port(), "/s/test.txt");
	REQUIRE(resp1.starts_with("HTTP/1.1 200 OK"));
	auto last_modified = extract_header(resp1, "Last-Modified");
	REQUIRE(!last_modified.empty());

	auto resp2 =
		conflux::tests::http_get_on(srv.port(), "/s/test.txt", std::format("If-Modified-Since: {}\r\n", last_modified));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));

	srv.stop();
	::unlink((std::string{tmpdir} + "/test.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: directory request without listing returns 403") {
	char tmpdir[] = "/tmp/conflux_dirlist_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "file.txt", "hi");

	conflux::http::Router router;
	router.serve_static("/s", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));

	srv.stop();
	::unlink((std::string{tmpdir} + "/file.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: directory request with listing returns HTML") {
	char tmpdir[] = "/tmp/conflux_dirlist2_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "alpha.txt", "a");
	write_file(tmpdir, "a&b<q\".txt", "escaped");
	write_file(tmpdir, "beta.html", "b");

	conflux::http::Router router;
	conflux::http::StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", std::string{tmpdir}, sopts);
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body = response_body(resp);
	REQUIRE(body.find("alpha.txt") != std::string_view::npos);
	REQUIRE(body.find("a&amp;b&lt;q&quot;.txt") != std::string_view::npos);
	REQUIRE(body.find("beta.html") != std::string_view::npos);
	REQUIRE(body.find("<ul>") != std::string_view::npos);

	srv.stop();
	::unlink((std::string{tmpdir} + "/alpha.txt").c_str());
	::unlink((std::string{tmpdir} + "/a&b<q\".txt").c_str());
	::unlink((std::string{tmpdir} + "/beta.html").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: directory listing entries are sorted") {
	char tmpdir[] = "/tmp/conflux_dirlist3_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "zebra.txt", "");
	write_file(tmpdir, "apple.txt", "");
	write_file(tmpdir, "mango.txt", "");

	conflux::http::Router router;
	conflux::http::StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", std::string{tmpdir}, sopts);
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body = response_body(resp);
	auto apple_pos = body.find("apple.txt");
	auto mango_pos = body.find("mango.txt");
	auto zebra_pos = body.find("zebra.txt");
	REQUIRE(apple_pos != std::string_view::npos);
	REQUIRE(mango_pos != std::string_view::npos);
	REQUIRE(zebra_pos != std::string_view::npos);
	REQUIRE(apple_pos < mango_pos);
	REQUIRE(mango_pos < zebra_pos);

	srv.stop();
	::unlink((std::string{tmpdir} + "/zebra.txt").c_str());
	::unlink((std::string{tmpdir} + "/apple.txt").c_str());
	::unlink((std::string{tmpdir} + "/mango.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: directory listing index.html takes precedence") {
	char tmpdir[] = "/tmp/conflux_dirlist4_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "index.html", "<h1>Index</h1>");

	conflux::http::Router router;
	conflux::http::StaticOptions sopts{};
	sopts.directory_listing = true;
	router.serve_static("/s", std::string{tmpdir}, sopts);
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = conflux::tests::http_get_on(srv.port(), "/s/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(response_body(resp) == "<h1>Index</h1>");

	srv.stop();
	::unlink((std::string{tmpdir} + "/index.html").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving") {
	char tmpdir[] = "/tmp/conflux_static_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "hello.txt", "Hello, static!");
	write_file(tmpdir, "page.html", "<h1>Static HTML</h1>");
	write_file(tmpdir, "data.json", R"({"key":"value"})");

	conflux::http::Router router;
	router.serve_static("/static", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};
	auto const port = srv.port();

	SECTION("serves .txt file with correct MIME type") {
		auto resp = http_get_on(port, "/static/hello.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(resp.find("Content-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
		REQUIRE(response_body(resp) == "Hello, static!");
	}

	SECTION("serves .html file with correct MIME type") {
		auto resp = http_get_on(port, "/static/page.html");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(resp.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos);
		REQUIRE(response_body(resp) == "<h1>Static HTML</h1>");
	}

	SECTION("serves .json file with correct MIME type") {
		auto resp = http_get_on(port, "/static/data.json");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(resp.find("Content-Type: application/json\r\n") != std::string::npos);
	}

	SECTION("returns ETag header") {
		auto resp = http_get_on(port, "/static/hello.txt");
		REQUIRE(resp.find("ETag: \"") != std::string::npos);
	}

	SECTION("returns 304 when If-None-Match matches ETag") {
		auto resp1 = http_get_on(port, "/static/hello.txt");
		auto etag = extract_header(resp1, "ETag");
		REQUIRE(!etag.empty());

		auto resp2 = get_with_extra_header(port, "/static/hello.txt", std::format("If-None-Match: {}\r\n", etag));
		REQUIRE(resp2.starts_with("HTTP/1.1 304 Not Modified"));
	}

	SECTION("returns 304 for If-Modified-Since using GMT under non-UTC TZ") {
		struct TzGuard {
			std::optional<std::string> old_tz;
			TzGuard() {
				if (char const *tz = std::getenv("TZ"); tz != nullptr) {
					old_tz = tz;
				}
				::setenv("TZ", "Asia/Tokyo", 1);
				::tzset();
			}
			~TzGuard() {
				if (old_tz) {
					::setenv("TZ", old_tz->c_str(), 1);
				} else {
					::unsetenv("TZ");
				}
				::tzset();
			}
		} const tz_guard;

		auto resp1 = http_get_on(port, "/static/hello.txt");
		auto last_modified = extract_header(resp1, "Last-Modified");
		REQUIRE(!last_modified.empty());

		auto resp2 =
			get_with_extra_header(port, "/static/hello.txt", std::format("If-Modified-Since: {}\r\n", last_modified));
		REQUIRE(resp2.starts_with("HTTP/1.1 304 Not Modified"));
	}

	SECTION("returns 404 for missing file") {
		auto resp = http_get_on(port, "/static/missing.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	}

	SECTION("rejects path traversal with 403") {
		auto resp = http_get_on(port, "/static/../etc/passwd");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	srv.stop();
	for (auto const &name: {"hello.txt", "page.html", "data.json"}) {
		::unlink((std::string{tmpdir} + "/" + name).c_str());
	}
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: root_dir with trailing slash works") {
	char tmpdir[] = "/tmp/conflux_static_slash_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);
	write_file(tmpdir, "hello.txt", "Hello, slash!");

	conflux::http::Router router;
	router.serve_static("/static", std::string{tmpdir} + "/");
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = http_get_on(srv.port(), "/static/hello.txt");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	CHECK(response_body(resp) == "Hello, slash!");

	srv.stop();
	::unlink((std::string{tmpdir} + "/hello.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: non-regular file is rejected") {
	char tmpdir[] = "/tmp/conflux_static_fifo_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	auto fifo_path = std::string{tmpdir} + "/pipe.txt";
	REQUIRE(::mkfifo(fifo_path.c_str(), 0600) == 0);

	conflux::http::Router router;
	router.serve_static("/static", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = http_get_on(srv.port(), "/static/pipe.txt");
	REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));

	srv.stop();
	::unlink(fifo_path.c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: trailing slash root_dir works for put and delete") {
	char tmpdir[] = "/tmp/conflux_static_slash_rw_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	conflux::http::Router router;
	conflux::http::StaticOptions sopts{};
	sopts.allow_put = true;
	sopts.allow_delete = true;
	router.serve_static("/static", std::string{tmpdir} + "/", sopts);
	ScopedTestServer srv{mw_config(), std::move(router)};
	auto const port = srv.port();

	SECTION("PUT works with trailing-slash root_dir") {
		auto resp = raw_request_on(port, "PUT", "/static/new.txt", "hello");
		REQUIRE(resp.starts_with("HTTP/1.1 201 Created"));
		auto path = std::string{tmpdir} + "/new.txt";
		int const fd = ::open(path.c_str(), O_RDONLY);
		REQUIRE(fd >= 0);
		char buf[16]{};
		auto n = ::read(fd, buf, sizeof(buf));
		::close(fd);
		REQUIRE(n == 5);
		CHECK(std::string_view{buf, static_cast<std::size_t>(n)} == "hello");
		::unlink(path.c_str());
	}

	SECTION("DELETE works with trailing-slash root_dir") {
		auto path = std::string{tmpdir} + "/gone.txt";
		write_file(tmpdir, "gone.txt", "bye");

		auto resp = raw_request_on(port, "DELETE", "/static/gone.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 204 No Content"));
		CHECK(::access(path.c_str(), F_OK) != 0);
	}

	srv.stop();
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: offload_pool parity") {
	char tmpdir[] = "/tmp/conflux_static_off_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "hello.txt", "Hello, offloaded!");
	write_file(tmpdir, "empty.txt", "");

	auto pool = std::make_shared<conflux::work::WorkPool>();

	conflux::http::Router router;
	conflux::http::StaticOptions sopts{};
	sopts.offload_pool = pool;
	router.serve_static("/static", std::string{tmpdir}, sopts);
	ScopedTestServer srv{mw_config(), std::move(router)};
	auto const port = srv.port();

	SECTION("offloaded GET returns body via pool") {
		auto resp = http_get_on(port, "/static/hello.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(response_body(resp) == "Hello, offloaded!");
	}

	SECTION("offloaded 404 for missing") {
		auto resp = http_get_on(port, "/static/missing.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	}

	SECTION("offloaded 403 for traversal") {
		auto resp = http_get_on(port, "/static/../etc/passwd");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	SECTION("offloaded 304 If-None-Match round-trip") {
		auto resp1 = http_get_on(port, "/static/hello.txt");
		auto etag = extract_header(resp1, "ETag");
		REQUIRE(!etag.empty());
		auto resp2 = get_with_extra_header(port, "/static/hello.txt", std::format("If-None-Match: {}\r\n", etag));
		REQUIRE(resp2.starts_with("HTTP/1.1 304 Not Modified"));
	}

	SECTION("offloaded zero-size file") {
		auto resp = http_get_on(port, "/static/empty.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
		REQUIRE(response_body(resp).empty());
	}

	SECTION("many concurrent offloaded requests") {
		constexpr int kClients = 32;
		std::vector<std::jthread> threads;
		std::atomic<int> ok{0};
		threads.reserve(kClients);
		for (int i = 0; i < kClients; ++i) {
			threads.emplace_back([&] {
				for (int attempt = 0; attempt < 3; ++attempt) {
					auto resp = http_get_on(port, "/static/hello.txt");
					if (resp.starts_with("HTTP/1.1 200 OK") && resp.find("Hello, offloaded!") != std::string::npos) {
						ok.fetch_add(1);
						return;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			});
		}
		threads.clear();
		REQUIRE(ok.load() == kClients);
	}

	srv.stop();
	pool->stop();
	pool->wait();
	for (auto const &name: {"hello.txt", "empty.txt"}) {
		::unlink((std::string{tmpdir} + "/" + name).c_str());
	}
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file serving: allow_put creates and overwrites files") {
	char tmpdir[] = "/tmp/conflux_static_put_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	conflux::http::Router router;
	conflux::http::StaticOptions sopts{};
	sopts.allow_put = true;
	router.serve_static("/static", std::string{tmpdir}, sopts);
	ScopedTestServer srv{mw_config(), std::move(router)};
	auto const port = srv.port();

	SECTION("PUT new file returns 201 Created") {
		auto resp = raw_request_on(port, "PUT", "/static/new.txt", "hello");
		REQUIRE(resp.starts_with("HTTP/1.1 201 Created"));
		auto path = std::string{tmpdir} + "/new.txt";
		int const fd = ::open(path.c_str(), O_RDONLY);
		REQUIRE(fd >= 0);
		char buf[16]{};
		auto n = ::read(fd, buf, sizeof(buf));
		::close(fd);
		REQUIRE(n == 5);
		CHECK(std::string_view{buf, static_cast<std::size_t>(n)} == "hello");
		::unlink(path.c_str());
	}

	SECTION("PUT existing file returns 204 No Content") {
		write_file(tmpdir, "existing.txt", "old");
		auto path = std::string{tmpdir} + "/existing.txt";

		auto resp = raw_request_on(port, "PUT", "/static/existing.txt", "new-content");
		REQUIRE(resp.starts_with("HTTP/1.1 204 No Content"));
		::unlink(path.c_str());
	}

	SECTION("PUT with path traversal returns 403") {
		auto resp = raw_request_on(port, "PUT", "/static/../escape.txt", "data");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	srv.stop();
	::rmdir(tmpdir);
}

TEST_CASE(
	"static core: normalize_static_path resolves dot segments and rejects escapes") {
	CHECK(conflux::http::detail::normalize_static_path("foo/bar") == std::optional<std::string>{"/foo/bar"});
	CHECK(conflux::http::detail::normalize_static_path("./foo/../bar") == std::optional<std::string>{"/bar"});
	CHECK_FALSE(conflux::http::detail::normalize_static_path("../escape").has_value());
	CHECK_FALSE(conflux::http::detail::normalize_static_path(std::string_view{"bad\0path", 8}).has_value());
}

TEST_CASE(
	"static file serving: allow_delete removes files") {
	char tmpdir[] = "/tmp/conflux_static_del_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	conflux::http::Router router;
	conflux::http::StaticOptions sopts{};
	sopts.allow_delete = true;
	router.serve_static("/static", std::string{tmpdir}, sopts);
	ScopedTestServer srv{mw_config(), std::move(router)};
	auto const port = srv.port();

	SECTION("DELETE existing file returns 204 No Content") {
		write_file(tmpdir, "todelete.txt", "bye");
		auto path = std::string{tmpdir} + "/todelete.txt";

		auto resp = raw_request_on(port, "DELETE", "/static/todelete.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 204 No Content"));
		CHECK(::access(path.c_str(), F_OK) != 0);
	}

	SECTION("DELETE missing file returns 404") {
		auto resp = raw_request_on(port, "DELETE", "/static/nonexistent.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 404 Not Found"));
	}

	SECTION("DELETE with path traversal returns 403") {
		auto resp = raw_request_on(port, "DELETE", "/static/../escape.txt");
		REQUIRE(resp.starts_with("HTTP/1.1 403 Forbidden"));
	}

	srv.stop();
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file: HEAD returns 200 with correct Content-Length but no body") {
	char tmpdir[] = "/tmp/conflux_head_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "hello.txt", "Hello, static!");

	conflux::http::Router router;
	router.serve_static("/f", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = head_static_on(srv.port(), "/f/hello.txt");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("\r\nContent-Length: 14\r\n") != std::string::npos);
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	REQUIRE(resp.size() == hdr_end + 4);

	srv.stop();
	::unlink((std::string{tmpdir} + "/hello.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file: Range request returns 206 with partial body") {
	char tmpdir[] = "/tmp/conflux_range_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "data.txt", "0123456789");

	conflux::http::Router router;
	router.serve_static("/f", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = get_with_extra_header(srv.port(), "/f/data.txt", "Range: bytes=2-5\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 206 Partial Content"));
	REQUIRE(resp.find("Content-Range: bytes 2-5/10") != std::string::npos);
	REQUIRE(response_body(resp) == "2345");

	srv.stop();
	::unlink((std::string{tmpdir} + "/data.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file: Range beyond file size returns 416") {
	char tmpdir[] = "/tmp/conflux_range416_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "tiny.txt", "hi");

	conflux::http::Router router;
	router.serve_static("/f", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = get_with_extra_header(srv.port(), "/f/tiny.txt", "Range: bytes=100-200\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 416"));

	srv.stop();
	::unlink((std::string{tmpdir} + "/tiny.txt").c_str());
	::rmdir(tmpdir);
}

TEST_CASE(
	"static file: suffix Range (bytes=-N) returns last N bytes") {
	char tmpdir[] = "/tmp/conflux_suffix_range_XXXXXX";
	REQUIRE(::mkdtemp(tmpdir) != nullptr);

	write_file(tmpdir, "data.txt", "0123456789");

	conflux::http::Router router;
	router.serve_static("/f", std::string{tmpdir});
	ScopedTestServer srv{mw_config(), std::move(router)};

	auto resp = get_with_extra_header(srv.port(), "/f/data.txt", "Range: bytes=-5\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 206 Partial Content"));
	REQUIRE(resp.find("Content-Range: bytes 5-9/10") != std::string::npos);
	REQUIRE(response_body(resp) == "56789");

	srv.stop();
	::unlink((std::string{tmpdir} + "/data.txt").c_str());
	::rmdir(tmpdir);
}
