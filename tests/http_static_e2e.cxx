#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import conflux.net.config;
import conflux.net.router;
import conflux.tests.support;

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
