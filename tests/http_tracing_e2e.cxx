#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.config;
import conflux.net.router;
import conflux.net.tracing;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

std::uint16_t g_trace_port = 0;

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

void ensure_trace_server() {
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::tracing_middleware({.propagate_in_response = true}));
		router.get("/", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["traceparent"]});
		});
		g_trace_port = start_mw_server(mw_config(), std::move(router));
	});
}

} // namespace

TEST_CASE(
	"tracing: generates traceparent when none in request") {
	ensure_trace_server();
	auto resp = http_get_on(g_trace_port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp_header = extract_header(resp, "Traceparent");
	REQUIRE(!tp_header.empty());
	REQUIRE(tp_header.starts_with("00-"));
	REQUIRE(tp_header.size() == 55);
	REQUIRE(extract_body(resp) == tp_header);
}

TEST_CASE(
	"tracing: incoming traceparent preserves trace_id, generates new span_id") {
	ensure_trace_server();
	std::string_view incoming = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
	auto resp = http_get_on(g_trace_port, "/", std::format("traceparent: {}\r\n", incoming));
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	REQUIRE(tp.substr(3, 32) == "4bf92f3577b34da6a3ce929d0e0e4736");
	auto new_span = tp.substr(36, 16);
	REQUIRE(new_span != "00f067aa0ba902b7");
}

TEST_CASE(
	"tracing: malformed traceparent generates fresh trace_id") {
	ensure_trace_server();
	auto resp = http_get_on(g_trace_port, "/", "traceparent: bad-value\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	REQUIRE(tp.size() == 55);
	REQUIRE(tp.substr(0, 3) == "00-");
	REQUIRE(tp[35] == '-');
	REQUIRE(tp[52] == '-');
}

TEST_CASE(
	"tracing: non-hex chars in trace_id reject the incoming traceparent") {
	ensure_trace_server();
	auto resp =
		http_get_on(g_trace_port, "/", "traceparent: 00-ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ-0000000000000000-01\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto tp = extract_header(resp, "Traceparent");
	REQUIRE(!tp.empty());
	REQUIRE(tp.substr(3, 32) != "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ");
}

TEST_CASE(
	"tracing: propagate_in_response=false omits Traceparent response header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(conflux::http::tracing_middleware({.propagate_in_response = false}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	REQUIRE(extract_header(resp, "Traceparent").empty());
}

TEST_CASE(
	"tracing: on_end callback can add response header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::tracing_middleware({
				.on_end = [](conflux::http::OwnedRequest const &,
							 conflux::http::Response &res,
							 conflux::http::TracingContext const &ctx) { res.headers["X-Trace-Id"] = ctx.trace_id; },
				.propagate_in_response = false,
			}));
		router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto trace_id = extract_header(resp, "X-Trace-Id");
	REQUIRE(trace_id.size() == 32);
}

TEST_CASE(
	"tracing: on_start callback receives TraceContext and can inject header") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		conflux::http::Router router;
		router.use(
			conflux::http::tracing_middleware({
				.on_start =
					[](conflux::http::OwnedRequest &req, conflux::http::TracingContext const &ctx) {
						req.headers["x-injected-span"] = ctx.span_id;
					},
				.propagate_in_response = false,
			}));
		router.get("/", [](conflux::http::OwnedRequest const &req) {
			return conflux::http::Response::text(std::string{req.headers["x-injected-span"]});
		});
		port = start_mw_server(mw_config(), std::move(router));
	});
	auto resp = http_get_on(port, "/");
	REQUIRE(resp.starts_with("HTTP/1.1 200 OK"));
	auto body = extract_body(resp);
	REQUIRE(body.size() == 16);
}
