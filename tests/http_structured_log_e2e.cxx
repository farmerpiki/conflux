#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

import std;
import conflux.json;
import conflux.net.config;
import conflux.net.router;
import conflux.net.structured_log;
import conflux.tests.support;

using conflux::http::Config;
using namespace conflux::json;
using namespace conflux::tests;

namespace {

std::uint16_t g_slog_port = 0;
char g_slog_path[64]{};

Document require_json_text(
	std::string_view text) {
	auto doc = conflux::json::parse_copy(std::string{text});
	REQUIRE(doc.has_value());
	return std::move(*doc);
}

NodeRef require_json_pointer(
	Document const &doc,
	std::string_view pointer) {
	auto node = doc.root().at_pointer(pointer);
	REQUIRE(node.has_value());
	return *node;
}

void check_json_string_at(
	Document const &doc,
	std::string_view pointer,
	std::string_view expected) {
	auto node = require_json_pointer(doc, pointer);
	auto value = node.as_string();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}

void check_json_u64_at(
	Document const &doc,
	std::string_view pointer,
	std::uint64_t expected) {
	auto node = require_json_pointer(doc, pointer);
	auto value = node.as_u64();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}

void check_json_absent_at(
	Document const &doc,
	std::string_view pointer) {
	CHECK_FALSE(doc.root().at_pointer(pointer).has_value());
}

std::string read_log_file(
	char const *path) {
	int const fd = ::open(path, O_RDONLY);
	REQUIRE(fd >= 0);
	std::array<char, 4096> buf{};
	ssize_t const n = ::read(fd, buf.data(), buf.size() - 1);
	::close(fd);
	REQUIRE(n > 0);
	return std::string{buf.data(), static_cast<std::size_t>(n)};
}

void ensure_slog_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		std::strcpy(g_slog_path, "/tmp/conflux_slog_XXXXXX");
		int const tmp = ::mkstemp(g_slog_path);
		::close(tmp);

		conflux::http::Router router;
		router.use(conflux::http::structured_log_middleware({.log_file = g_slog_path, .app_name = "test"}));
		router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
		g_slog_port = start_mw_server(mw_config(), std::move(router));
	});
}

} // namespace

TEST_CASE(
	"structured_log: request is logged as a JSON line to file") {
	ensure_slog_server();
	auto resp = http_get_on(g_slog_port, "/ping");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	auto const doc = require_json_text(read_log_file(g_slog_path));
	check_json_string_at(doc, "/method", "GET");
	check_json_string_at(doc, "/path", "/ping");
	check_json_u64_at(doc, "/status", 200);
	check_json_string_at(doc, "/app", "test");
}

TEST_CASE(
	"structured_log: no app_name omits app field") {
	char path[64]{};
	std::strcpy(path, "/tmp/conflux_slog2_XXXXXX");
	int const tmp = ::mkstemp(path);
	REQUIRE(tmp >= 0);
	::close(tmp);

	Config cfg = mw_config();
	conflux::http::Router router;
	router.use(conflux::http::structured_log_middleware({.log_file = path}));
	router.get("/x", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("x"); });
	ScopedTestServer srv{cfg, std::move(router)};

	auto resp = http_get_on(srv.port(), "/x");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	auto const doc = require_json_text(read_log_file(path));
	::unlink(path);
	check_json_absent_at(doc, "/app");
	check_json_string_at(doc, "/path", "/x");
}

TEST_CASE(
	"structured_log: path with double-quote is JSON-escaped in log") {
	char path[64]{};
	std::strcpy(path, "/tmp/conflux_slog3_XXXXXX");
	int const tmp = ::mkstemp(path);
	REQUIRE(tmp >= 0);
	::close(tmp);

	Config cfg = mw_config();
	conflux::http::Router router;
	router.use(conflux::http::structured_log_middleware({.log_file = path, .app_name = "test"}));
	router.get("/{*path}", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	ScopedTestServer srv{cfg, std::move(router)};

	LocalTcpClient client{srv.port()};
	client.set_recv_timeout(std::chrono::seconds{5});
	std::string_view const raw_request =
		"GET /q\" HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Connection: close\r\n"
		"\r\n";
	REQUIRE(client.send(raw_request, MSG_NOSIGNAL) == static_cast<ssize_t>(raw_request.size()));
	auto resp = client.read_one_response();
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	auto const doc = require_json_text(read_log_file(path));
	::unlink(path);
	check_json_string_at(doc, "/app", "test");
	check_json_string_at(doc, "/path", "/q\"");
}

TEST_CASE(
	"conflux::http::make_access_log_middleware logs request lines via sink") {
	std::vector<std::string> lines;
	std::mutex lines_mtx;

	Config cfg = mw_config();
	conflux::http::Router router;
	router.use(conflux::http::make_access_log_middleware([&](std::string const &line) {
		std::scoped_lock lk{lines_mtx};
		lines.push_back(line);
	}));
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
	router.get("/missing", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::not_found(req.path);
	});

	ScopedTestServer srv{cfg, std::move(router)};

	auto _ = http_get_on(srv.port(), "/ping");
	_ = http_get_on(srv.port(), "/missing");
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	srv.stop();

	std::scoped_lock lk{lines_mtx};
	auto has = [&](std::string_view sub) {
		return std::ranges::any_of(lines, [&](std::string const &line) { return line.find(sub) != std::string::npos; });
	};
	REQUIRE(has("GET /ping 200"));
	REQUIRE(has("GET /missing 404"));
	REQUIRE(has("[20"));
}
