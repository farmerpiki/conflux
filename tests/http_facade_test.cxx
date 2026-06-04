#ifndef CONFLUX_WORK_QUEUE_STATS
	#define CONFLUX_WORK_QUEUE_STATS 0
#endif

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.http;
import conflux.http.extended;
import conflux.net.config;
import conflux.work;

namespace http = conflux::http;
using namespace conflux::http;
using conflux::http::SecretSource;
using conflux::http::SecretSourceKind;

static_assert(std::same_as<http::Config, conflux::http::Config>);
static_assert(std::same_as<http::HttpPressureMetrics, conflux::http::HttpPressureMetrics>);

TEST_CASE(
	"http facade: lifecycle and pressure vocabulary defaults are explicit",
	"[http.facade]") {
	http::DrainOptions drain{};
	CHECK(drain.deadline == std::chrono::milliseconds{30000});
	CHECK(drain.stop_accepting);
	CHECK(drain.close_idle);
	CHECK(drain.finish_requests);
	CHECK_FALSE(drain.finish_streams);
	CHECK(drain.websocket_policy == http::DrainStreamPolicy::close);
	CHECK(drain.sse_policy == http::DrainStreamPolicy::close);

	http::DrainReport report{};
	CHECK(report.accepted_before_stop == 0);
	CHECK_FALSE(report.deadline_hit);

	http::HttpServerMetrics metrics{};
	CHECK(metrics.pressure.accept_rejected == 0);
	CHECK(metrics.pressure.drain_started == 0);
	CHECK(metrics.pressure.drain_forced_close == 0);
}

TEST_CASE(
	"http facade: app state stores borrowed and shared typed state",
	"[http.facade]") {
	auto app = http::app();

	int borrowed = 42;
	app.state(borrowed);
	CHECK(app.state<int>().value == std::addressof(borrowed));
	CHECK(*app.state<int>() == 42);

	auto owned = std::make_shared<std::string>("owned");
	auto const *owned_ptr = owned.get();
	app.state(owned);
	auto owned_state = app.state<std::string>();
	REQUIRE(owned_state.value != nullptr);
	CHECK(owned_state.value == owned_ptr);
	CHECK(owned_state.get() == "owned");
	auto shared_state = app.state<std::shared_ptr<std::string>>();
	REQUIRE(shared_state.value != nullptr);
	CHECK(*shared_state.get() == "owned");
}

TEST_CASE(
	"http facade: app state ownership aliases are explicit",
	"[http.facade]") {
	auto app = http::app();

	int borrowed = 11;
	app.state_ref(borrowed);
	CHECK(app.state<int>().value == std::addressof(borrowed));
	CHECK(app.state<int>().get() == 11);

	app.state_owned(std::string{"owned"});
	auto owned_state = app.state<std::string>();
	REQUIRE(owned_state.value != nullptr);
	CHECK(owned_state.get() == "owned");

	auto shared = std::make_shared<double>(2.5);
	app.state_shared(shared);
	auto shared_state = app.state<std::shared_ptr<double>>();
	REQUIRE(shared_state.value != nullptr);
	CHECK(shared_state.get() == shared);
}

TEST_CASE(
	"http facade: server startup report is explicit and redacted",
	"[http.facade]") {
	auto cfg = http::Config::development();
	cfg.port = 0;
	cfg.startup_banner = false;
	cfg.request_timeout_ms = 1000;
	cfg.dump_effective_config = true;
	cfg.send_fixed_buffers = true;
	cfg.fixed_buffer_slabs = 1024 * 1024;
	cfg.fixed_buffer_bytes = 1024 * 1024;
	cfg.feature_fallback = conflux::runtime::FeatureFallback::silent_fallback;
	cfg.auth_secrets.jwt.active = SecretSource{.kind = SecretSourceKind::literal, .value = "super-secret-token"};
	auto app = http::App{cfg};
	app.get("/health", [] { return http::text("ok"); });

	auto server = std::move(app).try_server();
	if (!server) {
		FAIL_CHECK(server.error());
	}
	REQUIRE(server.has_value());
	auto report = (*server)->startup_report();
	CHECK(report.find("Build:") != std::string::npos);
	CHECK(report.find("Capabilities:") != std::string::npos);
	CHECK(report.find("Fallbacks:") != std::string::npos);
	CHECK(report.find("Config:") != std::string::npos);
	CHECK(report.find("\"server\"") != std::string::npos);
	CHECK(report.find("policy=silent_fallback") != std::string::npos);
	CHECK(report.find("super-secret-token") == std::string::npos);
}
