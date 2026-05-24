// Plain TU — not a module unit.
#include <catch2/catch_test_macros.hpp>
#include <cctype>

import std;
import conflux.net.app;
import conflux.net.config;
import conflux.net.http_server;
import conflux.net.observability;
import conflux.net.router;
import conflux.net.observability;
import conflux.tests.support;

using namespace conflux::tests;
namespace http = conflux::http;

namespace {

[[nodiscard]] std::string response_body(
	std::string_view response) {
	auto const pos = response.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{response.substr(pos + 4)};
}

[[nodiscard]] bool iequals(
	std::string_view a,
	std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		auto const ac = static_cast<unsigned char>(a[i]);
		auto const bc = static_cast<unsigned char>(b[i]);
		if (std::tolower(ac) != std::tolower(bc)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::string response_header(
	std::string_view response,
	std::string_view name) {
	auto const hdr_end = response.find("\r\n\r\n");
	if (hdr_end == std::string_view::npos) {
		return {};
	}
	std::size_t line = response.find("\r\n");
	if (line == std::string_view::npos || line + 2 >= hdr_end) {
		return {};
	}
	line += 2;
	while (line < hdr_end) {
		auto const next = response.find("\r\n", line);
		if (next == std::string_view::npos || next > hdr_end) {
			break;
		}
		auto const colon = response.find(':', line);
		if (colon != std::string_view::npos && colon < next) {
			auto const key = response.substr(line, colon - line);
			if (iequals(key, name)) {
				std::size_t value = colon + 1;
				while (value < next && response[value] == ' ') {
					++value;
				}
				return std::string{response.substr(value, next - value)};
			}
		}
		line = next + 2;
	}
	return {};
}

[[nodiscard]] std::vector<std::string> snapshot_logs(
	std::mutex &mu,
	std::vector<std::string> const &logs) {
	std::lock_guard const lock{mu};
	return logs;
}

void require_log_count_at_least(
	std::mutex &mu,
	std::vector<std::string> const &logs,
	std::size_t count) {
	for (int attempt = 0; attempt < 100; ++attempt) {
		if (snapshot_logs(mu, logs).size() >= count) {
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	}
	REQUIRE(snapshot_logs(mu, logs).size() >= count);
}

class ScopedAppServer {
public:
	explicit ScopedAppServer(
		http::App app) {
		auto server = std::move(app).try_server({.port = 0});
		REQUIRE(server.has_value());
		server_ = std::move(*server);
		thread_ = std::thread{[this] { (void)server_->run(); }};
		wait_for_server(server_->port());
	}
	~ScopedAppServer() { stop(); }
	ScopedAppServer(ScopedAppServer const &) = delete;
	ScopedAppServer &operator =(ScopedAppServer const &) = delete;
	[[nodiscard]] std::uint16_t port() const { return server_->port(); }
	void stop() {
		if (thread_.joinable()) {
			server_->request_shutdown();
			thread_.join();
		}
	}

private:
	std::unique_ptr<HttpServer> server_;
	std::thread thread_;
};

} // namespace

TEST_CASE(
	"observability golden e2e: request id tracing logs and route metrics stay coherent",
	"[observability][e2e]") {
	std::mutex logs_mu;
	std::vector<std::string> logs;

	auto cfg = mw_config();
	auto app = http::app(cfg);
	app.use(
		http::observability({
			.service_name = "golden-api",
			.log_request_headers = true,
			.log_response_headers = true,
			.extra_sensitive_headers = {"X-Secret", "X-Secret-Response"},
			.access_log_sink =
				[&](std::string const &line) {
					std::lock_guard const lock{logs_mu};
					logs.push_back(line);
										},
    }));
	app.get("/users/{id}", [](http::Path<"id"> id, http::RequestId request_id, http::TraceContext trace) {
		auto response =
			Response::text(std::format("id={} request={} trace={}", id.get(), request_id.get(), trace.traceparent));
		response.headers.set("X-Secret-Response", "response-secret");
		return response;
	});

	ScopedAppServer server{std::move(app)};
	std::string_view const incoming_trace = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
	auto const extra_headers = std::format(
		"X-Request-ID: golden-request-id\r\n"
		"traceparent: {}\r\n"
		"Authorization: Bearer request-secret\r\n"
		"Cookie: sid=cookie-secret\r\n"
		"X-Secret: custom-secret\r\n"
		"User-Agent: golden-test\r\n",
		incoming_trace);

	auto first = http_get_on(server.port(), "/users/42?debug=request-secret", extra_headers);
	REQUIRE(first.starts_with("HTTP/1.1 200 OK"));
	CHECK(response_header(first, "X-Request-ID") == "golden-request-id");
	auto const first_trace = response_header(first, "Traceparent");
	REQUIRE(first_trace.size() == 55);
	CHECK(first_trace.substr(3, 32) == "4bf92f3577b34da6a3ce929d0e0e4736");
	CHECK(first_trace.substr(36, 16) != "00f067aa0ba902b7");
	CHECK(response_body(first).contains("id=42"));
	CHECK(response_body(first).contains("request=golden-request-id"));
	CHECK(response_body(first).contains(first_trace));
	CHECK(response_header(first, "__conflux-route-pattern").empty());

	auto second = http_get_on(server.port(), "/users/43?debug=other-secret", extra_headers);
	REQUIRE(second.starts_with("HTTP/1.1 200 OK"));

	std::string metrics_body;
#if CONFLUX_HAS_METRICS
	auto metrics = http_get_on(server.port(), "/metrics");
	REQUIRE(metrics.starts_with("HTTP/1.1 200 OK"));
	metrics_body = response_body(metrics);
	CHECK(metrics_body.contains(
		R"(http_requests_total{service="golden-api",route="/users/{id}",method="GET",status_class="2xx",status="200"} 2)"));
	CHECK(metrics_body.contains(
		R"(http_request_duration_seconds_count{service="golden-api",route="/users/{id}",method="GET"} 2)"));
	CHECK(metrics_body.find(R"(route="/users/42")") == std::string::npos);
	CHECK(metrics_body.find(R"(route="/users/43")") == std::string::npos);
	CHECK(metrics_body.find("request-secret") == std::string::npos);
#endif

	require_log_count_at_least(logs_mu, logs, 2);
	auto const lines = snapshot_logs(logs_mu, logs);
	REQUIRE(lines.size() >= 2);
	auto const &line = lines.front();
	CHECK(line.contains(R"("event":"http_request")"));
	CHECK(line.contains(R"("service":"golden-api")"));
	CHECK(line.contains(R"("request_id":"golden-request-id")"));
	CHECK(line.contains(R"("trace_id":")"));
	CHECK(line.contains("4bf92f3577b34da6a3ce929d0e0e4736"));
	CHECK(line.contains(R"("method":"GET")"));
	CHECK(line.contains(R"("path":"/users/42")"));
	CHECK(line.contains(R"("route":"/users/{id}")"));
	CHECK(line.contains(R"("status":200)"));
	CHECK(line.contains(R"("status_class":"2xx")"));
	CHECK(line.contains(R"("user_agent":"golden-test")"));
	bool const has_authorization_redacted =
		line.contains(R"("authorization":"<redacted>")") || line.contains(R"("Authorization":"<redacted>")");
	bool const has_cookie_redacted =
		line.contains(R"("cookie":"<redacted>")") || line.contains(R"("Cookie":"<redacted>")");
	CHECK(has_authorization_redacted);
	CHECK(has_cookie_redacted);
	CHECK(line.contains("<redacted>"));
	CHECK(line.find("request-secret") == std::string::npos);
	CHECK(line.find("cookie-secret") == std::string::npos);
	CHECK(line.find("custom-secret") == std::string::npos);
	CHECK(line.find("response-secret") == std::string::npos);
	CHECK(line.find("debug=request-secret") == std::string::npos);
	CHECK(line.find("__conflux-route-pattern") == std::string::npos);
}
