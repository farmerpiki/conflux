#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.request_id;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::uint16_t g_rid_port = 0;

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

void ensure_rid_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::request_id_middleware());
		router.get("/", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["x-request-id"]});
		});
		g_rid_port = start_mw_server(mw_config(), std::move(router));
	});
}

} // namespace

TEST_CASE(
	"request_id: generates X-Request-ID when absent") {
	ensure_rid_server();
	auto resp = http_get_on(g_rid_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Request-ID:") != std::string::npos);
	auto body = extract_body(resp);
	REQUIRE(!body.empty());
	REQUIRE(body.size() == 36);
	REQUIRE(body[8] == '-');
	REQUIRE(body[13] == '-');
	REQUIRE(body[18] == '-');
	REQUIRE(body[23] == '-');
}

TEST_CASE(
	"request_id: echoes existing X-Request-ID from client") {
	ensure_rid_server();
	auto resp = http_get_on(g_rid_port, "/", "X-Request-ID: my-trace-id-123\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Request-ID: my-trace-id-123") != std::string::npos);
	REQUIRE(extract_body(resp) == "my-trace-id-123");
}

TEST_CASE(
	"request_id: two requests get different generated IDs") {
	ensure_rid_server();
	auto resp1 = http_get_on(g_rid_port, "/");
	auto resp2 = http_get_on(g_rid_port, "/");
	auto body1 = extract_body(resp1);
	auto body2 = extract_body(resp2);
	REQUIRE(body1 != body2);
}

TEST_CASE(
	"request_id: trust_incoming=false always generates fresh ID") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::request_id_middleware({.trust_incoming = false}));
		router.get("/", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["x-request-id"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/", "X-Request-ID: client-provided-id\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body = extract_body(resp);
	REQUIRE(body != "client-provided-id");
	REQUIRE(body.size() == 36);
}

TEST_CASE(
	"request_id: custom header name is stamped on request and response") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::request_id_middleware({.header = "X-Trace-ID"}));
		router.get("/", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["x-trace-id"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("X-Trace-ID:") != std::string::npos);
	REQUIRE(extract_body(resp).size() == 36);
}
