#ifndef CONFLUX_WORK_QUEUE_STATS
	#define CONFLUX_WORK_QUEUE_STATS 0
#endif

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.http;
import conflux.http.extended;
import conflux.json;
import conflux.net.observability;
import conflux.work;

namespace http = conflux::http;
using namespace conflux::http;
using namespace conflux::json;

conflux::json::Document require_json_text(
	std::string text) {
	INFO(text);
	auto doc = conflux::json::parse_copy(text);
	REQUIRE(doc.has_value());
	return std::move(*doc);
}

conflux::json::NodeRef require_json_pointer(
	conflux::json::Document const &doc,
	std::string_view pointer) {
	auto node = doc.root().at_pointer(pointer);
	REQUIRE(node.has_value());
	return *node;
}

void check_json_string_at(
	conflux::json::Document const &doc,
	std::string_view pointer,
	std::string_view expected) {
	auto node = require_json_pointer(doc, pointer);
	auto value = node.as_string();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}

void check_json_contains_no_secret(
	conflux::json::NodeRef node,
	std::span<std::string_view const> secrets) {
	if (auto value = node.as_string(); value.has_value()) {
		for (auto secret: secrets) {
			CHECK_FALSE(value->contains(secret));
		}
	}
	if (auto object = node.as_object(); object.has_value()) {
		for (auto const &[name, value]: object->members()) {
			for (auto secret: secrets) {
				CHECK_FALSE(name.contains(secret));
			}
			check_json_contains_no_secret(value, secrets);
		}
	}
	if (auto array = node.as_array(); array.has_value()) {
		for (auto value: array->elements()) {
			check_json_contains_no_secret(value, secrets);
		}
	}
}

TEST_CASE(
	"http facade: observability facade installs request id tracing logs and metrics",
	"[http.facade]") {
	auto app = http::app();
	std::vector<std::string> logs;
	app.use(
		http::observability({
			.service_name = "api",
			.log_request_headers = true,
			.extra_sensitive_headers = {"X-Secret"},
			.access_log_sink = [&](std::string const &line) { logs.push_back(line); },
		}));
	app.get("/items/{id}", [](http::RequestId request_id, http::TraceContext trace) {
		auto response = http::text(std::format("{} {}", request_id.get(), trace.traceparent));
		response.headers.set("Set-Cookie", "session=abc");
		return response;
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/items/42?debug=true";
	req.headers.set("Authorization", "Bearer secret");
	req.headers.set("X-Secret", "secret");

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.headers.get("X-Request-ID").has_value());
	CHECK(response.headers.get("Traceparent").has_value());
	REQUIRE(logs.size() == 1);
	auto const log_doc = require_json_text(logs[0]);
	check_json_string_at(log_doc, "/service", "api");
	check_json_string_at(log_doc, "/route", "/items/{id}");
	check_json_string_at(log_doc, "/path", "/items/42");
	check_json_string_at(log_doc, "/request_headers/Authorization", "<redacted>");
	check_json_string_at(log_doc, "/request_headers/X-Secret", "<redacted>");
	std::array<std::string_view, 2> const secrets{"debug=true", "Bearer secret"};
	check_json_contains_no_secret(log_doc.root(), secrets);

	conflux::http::OwnedRequest metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	CHECK(metrics.status == kHttpOk);
	CHECK(metrics.text_body().contains("http_requests_total"));
	CHECK(metrics.text_body().contains(
		R"(service="api",route="/items/{id}",method="GET",status_class="2xx",status="200")"));
	CHECK(metrics.text_body().contains("http_request_duration_seconds_count"));
	CHECK_FALSE(metrics.text_body().contains("debug=true"));
}

TEST_CASE(
	"http facade: observability metrics route can be disabled and collision is validated",
	"[http.facade]") {
	auto disabled = http::app();
	disabled.use(http::observability({.access_log = false, .register_metrics_route = false}));
	CHECK(disabled.routes().empty());

	auto colliding = http::app();
	colliding.get("/metrics", [] { return http::text("custom"); });
	colliding.use(http::observability({.access_log = false}));

	auto report = colliding.validate();
	REQUIRE_FALSE(report.ok());
	CHECK(std::ranges::any_of(report.issues, [](auto const &issue) {
		return issue.method == "GET" && issue.path == "/metrics" && issue.message == "duplicate route";
	}));
}

TEST_CASE(
	"http facade: observability records unmatched route metrics and logs",
	"[http.facade]") {
	auto app = http::app();
	std::vector<std::string> logs;
	app.use(
		http::observability({
			.service_name = "api",
			.access_log_sink = [&](std::string const &line) { logs.push_back(line); },
		}));

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/missing?token=secret";
	auto missing = http::router(app).dispatch(req);
	CHECK(missing.status == kHttpNotFound);
	REQUIRE(logs.size() == 1);
	auto const log_doc = require_json_text(logs[0]);
	check_json_string_at(log_doc, "/route", "<unmatched>");
	std::array<std::string_view, 1> const secrets{"token=secret"};
	check_json_contains_no_secret(log_doc.root(), secrets);

	conflux::http::OwnedRequest metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	CHECK(metrics.text_body().contains(R"(route="<unmatched>")"));
	CHECK(metrics.text_body().contains("http_rejections_total"));
}

TEST_CASE(
	"http facade: observability metrics include explicit runtime and work sources",
	"[http.facade]") {
	auto pool = std::make_shared<http::WorkPool>(http::WorkPoolOptions{.threads = 1, .max_inject_queue = 16});
	std::atomic<int> ran{0};
	REQUIRE(pool->enqueue([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
	pool->drain_and_stop();
	CHECK(ran.load(std::memory_order_relaxed) == 1);

	http::HttpPressureMetrics pressure{};
	pressure.accept_rejected = 2;
	pressure.sse_dropped_newest = 3;
	pressure.websocket_closed_for_pressure = 4;

	auto app = http::app();
	app.use(
		http::observability(
			{
				.service_name = "api",
				.access_log = false,
				.task_allocation_metrics = true,
				.work_pools = {{"default", pool}},
			},
			http::ObservabilitySinks{
				.pressure_metrics = [pressure] { return pressure; },
				.json_arena_metrics =
					[] {
						return http::JsonArenaMetrics{
							.slabs_total = 5,
							.high_water_bytes = 4096,
							.allocated_bytes = 2048};
					},
			}));

	conflux::http::OwnedRequest metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	auto const body = metrics.text_body();
	CHECK(body.contains(R"(http_pressure_overflow_total{kind="accept",policy="reject"} 2)"));
	CHECK(body.contains(R"(http_pressure_overflow_total{kind="sse",policy="drop_newest"} 3)"));
	CHECK(body.contains(std::format("work_pool_queue_stats_enabled {}", CONFLUX_WORK_QUEUE_STATS ? 1 : 0)));
	CHECK(body.contains(R"(work_pool_completed_total{pool="default"})"));
	CHECK(body.contains("work_task_frame_allocations_total"));
	CHECK_FALSE(body.contains("json_arena_slabs_total"));
}

TEST_CASE(
	"http facade: observability JSON arena metrics are opt-in",
	"[http.facade]") {
	auto app = http::app();
	app.use(
		http::observability(
			{
				.access_log = false,
				.json_arena_metrics = true,
			},
			http::ObservabilitySinks{
				.json_arena_metrics =
					[] {
						return http::JsonArenaMetrics{
							.slabs_total = 2,
							.high_water_bytes = 256,
							.allocated_bytes = 128};
					},
			}));

	conflux::http::OwnedRequest metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	auto const body = metrics.text_body();
	CHECK(body.contains("json_arena_slabs_total 2"));
	CHECK(body.contains("json_arena_high_water_bytes 256"));
	CHECK(body.contains("json_arena_allocated_bytes 128"));
}

TEST_CASE(
	"http facade: observability server rejection hook shares metrics registry",
	"[http.facade]") {
	auto middleware = http::observability({.access_log = false});
	auto hooks = http::observability_server_hooks(middleware);
	REQUIRE(static_cast<bool>(hooks.rejection));
	hooks.rejection(
		http::HttpRejectReason::header_line_too_large,
		http::reject_reason_status(http::HttpRejectReason::header_line_too_large));

	auto app = http::app();
	app.use(middleware);

	conflux::http::OwnedRequest metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	CHECK(metrics.text_body().contains(
		R"(http_rejections_total{service="conflux",reason="header_line_too_large",status="431"} 1)"));
}
