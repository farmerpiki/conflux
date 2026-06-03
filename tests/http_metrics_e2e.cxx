#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.auth;
import conflux.net.config;
import conflux.net.metrics;
import conflux.net.router;
import conflux.tests.support;

using conflux::http::Config;
using namespace conflux::tests;

namespace {

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

std::uint16_t g_metrics_port = 0;

void ensure_metrics_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		static conflux::http::MetricsRegistry reg;
		router.use(conflux::http::metrics_middleware(reg));
		router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
		router.get("/metrics", conflux::http::metrics_handler(reg));
		g_metrics_port = test_servers().start(cfg, std::move(router));
	});
}

std::uint16_t g_protected_metrics_port = 0;

void ensure_protected_metrics_server() {
	static std::once_flag once;
	std::call_once(once, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		static conflux::http::MetricsRegistry reg2;
		router.use(conflux::http::metrics_middleware(reg2));
		std::vector<conflux::http::Router::Middleware> chain;
		chain.push_back(
			conflux::http::bearer_auth_middleware([](std::string_view token) { return token == "supersecret"; }));
		router.get("/metrics", conflux::http::metrics_handler_protected(reg2, std::move(chain)));
		g_protected_metrics_port = test_servers().start(cfg, std::move(router));
	});
}

} // namespace

TEST_CASE(
	"metrics: Counter inc and get") {
	conflux::http::Counter c;
	CHECK(c.get() == 0);
	c.inc();
	CHECK(c.get() == 1);
	c.inc(5);
	CHECK(c.get() == 6);
}

TEST_CASE(
	"metrics: Gauge set inc dec") {
	conflux::http::Gauge g;
	g.set(10.0);
	CHECK(g.get() == 10.0);
	g.inc(2.5);
	CHECK(g.get() == 12.5);
	g.dec(3.0);
	CHECK(g.get() == 9.5);
}

TEST_CASE(
	"metrics: Histogram observe updates count and sum") {
	conflux::http::Histogram h;
	h.observe(0.1);
	h.observe(0.05);
	CHECK(h.count() == 2);
	CHECK(h.sum() > 0.14);
	CHECK(h.sum() < 0.16);
}

TEST_CASE(
	"metrics: Histogram bucket boundaries") {
	conflux::http::Histogram h;
	h.observe(0.003);
	h.observe(0.007);
	CHECK(h.bucket(0) == 1);
	CHECK(h.bucket(1) == 2);
}

TEST_CASE(
	"metrics: pressure counters render as Prometheus events") {
	conflux::http::HttpPressureMetrics pressure{};
	pressure.accept_rejected = 2;
	pressure.drain_started = 1;
	pressure.drain_deadline_hit = 1;
	pressure.websocket_closed_for_pressure = 3;

	auto out = conflux::http::format_pressure_metrics_prometheus(pressure);
	CHECK(out.find("# TYPE http_pressure_events_total counter") != std::string::npos);
	CHECK(out.find("http_pressure_events_total{event=\"accept_rejected\"} 2") != std::string::npos);
	CHECK(out.find("http_pressure_events_total{event=\"drain_started\"} 1") != std::string::npos);
	CHECK(out.find("http_pressure_events_total{event=\"drain_deadline_hit\"} 1") != std::string::npos);
	CHECK(out.find("http_pressure_events_total{event=\"websocket_closed_for_pressure\"} 3") != std::string::npos);
}

TEST_CASE(
	"metrics: /ping increments http_requests_total GET 2xx") {
	ensure_metrics_server();
	http_get_on(g_metrics_port, "/ping");
	http_get_on(g_metrics_port, "/ping");
	auto resp = http_get_on(g_metrics_port, "/metrics");
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto body = resp.substr(hdr_end + 4);
	REQUIRE(body.find("http_requests_total{method=\"GET\",status=\"2xx\"}") != std::string::npos);
}

TEST_CASE(
	"metrics: /metrics returns Prometheus content-type") {
	ensure_metrics_server();
	auto resp = http_get_on(g_metrics_port, "/metrics");
	REQUIRE(resp.find("text/plain; version=0.0.4") != std::string::npos);
}

TEST_CASE(
	"metrics_handler_protected: missing bearer token returns 401") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
	REQUIRE(resp.find("WWW-Authenticate: Bearer") != std::string::npos);
}

TEST_CASE(
	"metrics_handler_protected: wrong bearer token returns 401") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics", "Authorization: Bearer badguess\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 401"));
}

TEST_CASE(
	"metrics_handler_protected: valid bearer token returns 200 prometheus body") {
	ensure_protected_metrics_server();
	auto resp = http_get_on(g_protected_metrics_port, "/metrics", "Authorization: Bearer supersecret\r\n");
	REQUIRE(resp.starts_with("HTTP/1.1 200"));
	REQUIRE(resp.find("text/plain; version=0.0.4") != std::string::npos);
	REQUIRE(resp.find("http_requests_total") != std::string::npos);
}

TEST_CASE(
	"metrics: duration histogram appears in output") {
	ensure_metrics_server();
	http_get_on(g_metrics_port, "/ping");
	auto resp = http_get_on(g_metrics_port, "/metrics");
	auto hdr_end = resp.find("\r\n\r\n");
	REQUIRE(hdr_end != std::string::npos);
	auto body = resp.substr(hdr_end + 4);
	REQUIRE(body.find("http_request_duration_seconds_sum") != std::string::npos);
	REQUIRE(body.find("http_request_duration_seconds_count") != std::string::npos);
	REQUIRE(body.find("http_request_duration_seconds_bucket") != std::string::npos);
}

TEST_CASE(
	"metrics: 4xx response increments GET 4xx counter") {
	static std::uint16_t port = 0;
	static std::once_flag flag;
	std::call_once(flag, [] {
		Config const cfg{.port = 0, .rings = 1};
		conflux::http::Router router;
		static conflux::http::MetricsRegistry reg;
		router.use(conflux::http::metrics_middleware(reg));
		router.get("/ok", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
		router.get("/metrics", conflux::http::metrics_handler(reg));
		port = test_servers().start(cfg, std::move(router));
	});
	http_get_on(port, "/nonexistent");
	auto resp = http_get_on(port, "/metrics");
	auto body = extract_body(resp);
	REQUIRE(body.find("http_requests_total{method=\"GET\",status=\"4xx\"}") != std::string::npos);
}

TEST_CASE(
	"metrics: 5xx response increments GET 5xx counter") {
	conflux::http::MetricsRegistry reg;
	reg.record("GET", 500, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"GET\",status=\"5xx\"}") != std::string::npos);
}

TEST_CASE(
	"metrics: OTHER method bucket used for non-standard methods") {
	conflux::http::MetricsRegistry reg;
	reg.record("PURGE", 200, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"OTHER\",status=\"2xx\"}") != std::string::npos);
}

TEST_CASE(
	"metrics: out-of-range status maps to other bucket") {
	conflux::http::MetricsRegistry reg;
	reg.record("GET", 999, std::chrono::milliseconds{1});
	auto out = reg.format_prometheus();
	REQUIRE(out.find("http_requests_total{method=\"GET\",status=\"other\"}") != std::string::npos);
}
