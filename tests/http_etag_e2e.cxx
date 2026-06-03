#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.etag;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::uint16_t g_etag_port = 0;

std::string extract_header(
	std::string_view resp,
	std::string_view name) {
	auto needle = std::string{"\r\n"} + std::string{name} + ": ";
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

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

void ensure_etag_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::etag_middleware());
		router.get("/content", [](conflux::http::OwnedRequest const &) {
			return conflux::http::Response::text("hello world");
		});
		router.get("/empty", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
		g_etag_port = start_mw_server(mw_config(), std::move(router));
	});
}

} // namespace

TEST_CASE(
	"etag: response gets ETag header") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/content");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("ETag: \"") != std::string::npos);
}

TEST_CASE(
	"etag: If-None-Match matching ETag returns 304") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", etag));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
	CHECK(extract_header(resp2, "ETag") == etag);
}

TEST_CASE(
	"etag: If-None-Match wildcard returns 304") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto resp = http_get_on(g_etag_port, "/content", "If-None-Match: *\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 304"));
	CHECK(extract_header(resp, "ETag") == etag);
}

TEST_CASE(
	"etag: If-None-Match uses weak comparison") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: W/{}\r\n", etag));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
	REQUIRE(extract_header(resp2, "ETag") == etag);
}

TEST_CASE(
	"etag: If-None-Match non-matching tag returns 200 with body") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/content", "If-None-Match: \"000000\"\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_body(resp) == "hello world");
}

TEST_CASE(
	"etag: empty body response has no ETag") {
	ensure_etag_server();
	auto resp = http_get_on(g_etag_port, "/empty");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(resp.find("ETag:") == std::string::npos);
}

TEST_CASE(
	"etag: two requests to same route return same ETag") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto resp2 = http_get_on(g_etag_port, "/content");
	auto etag1 = extract_header(resp1, "ETag");
	auto etag2 = extract_header(resp2, "ETag");
	REQUIRE(!etag1.empty());
	REQUIRE(etag1 == etag2);
}

TEST_CASE(
	"etag: weak option produces W/ prefix") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::etag_middleware({.weak = true}));
		router.get("/w", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("body"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/w");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto etag = extract_header(resp, "ETag");
	REQUIRE(!etag.empty());
	REQUIRE(etag.starts_with("W/\""));
}

TEST_CASE(
	"etag: If-None-Match with multiple values matches correct ETag") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto inm = std::format("\"deadbeef\", {}, \"cafebabe\"", etag);
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", inm));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
}

TEST_CASE(
	"etag: weak If-None-Match matches strong ETag (RFC 7232 weak comparison)") {
	ensure_etag_server();
	auto resp1 = http_get_on(g_etag_port, "/content");
	auto etag = extract_header(resp1, "ETag");
	REQUIRE(!etag.empty());
	auto weak_inm = std::string{"W/"} + etag;
	auto resp2 = http_get_on(g_etag_port, "/content", std::format("If-None-Match: {}\r\n", weak_inm));
	REQUIRE(resp2.starts_with("HTTP/1.1 304"));
}

TEST_CASE(
	"etag: handler-set ETag is not overwritten by middleware") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router r;
		r.use(conflux::http::etag_middleware());
		r.get("/custom", [](conflux::http::OwnedRequest const &) {
			auto resp = conflux::http::Response::text("body");
			resp.headers["ETag"] = "\"custom-etag-42\"";
			return resp;
		});
		port = start_mw_server(mw_config(), std::move(r));
	});
	auto resp = http_get_on(port, "/custom");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_header(resp, "ETag") == "\"custom-etag-42\"");
}
