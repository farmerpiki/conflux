module;
#include <cstdint>

export module conflux.net.metrics;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.router;
import conflux.net.http.response;
import conflux.utils;
export namespace conflux::http {
// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

// Atomic counter (monotonically increasing, std::thread-safe).
class Counter {
	std::atomic<std::uint64_t> value_;

public:
	void inc() noexcept { inc(1); }
	void inc(
		std::uint64_t n) noexcept {
		value_.fetch_add(n, std::memory_order_relaxed);
	}
	[[nodiscard]] std::uint64_t get() const noexcept { return value_.load(std::memory_order_relaxed); }
};
// Gauge (current value, can go up or down).
class Gauge {
	std::atomic<double> value_;

public:
	void set(
		double v) noexcept {
		value_.store(v, std::memory_order_relaxed);
	}
	void inc(
		double n) noexcept {
		value_.fetch_add(n, std::memory_order_relaxed);
	}
	void dec(
		double n) noexcept {
		value_.fetch_sub(n, std::memory_order_relaxed);
	}
	[[nodiscard]] double get() const noexcept { return value_.load(std::memory_order_relaxed); }
};
// Fixed-bucket histogram for latency measurements.
// Buckets are upper bounds in seconds (standard Prometheus latency buckets).
class Histogram {
public:
	static constexpr std::array<double, 11> kBuckets = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
	void observe(
		double seconds) noexcept {
		sum_.fetch_add(seconds, std::memory_order_relaxed);
		count_.fetch_add(1, std::memory_order_relaxed);
		for (std::size_t i = 0; i < kBuckets.size(); ++i) {
			if (seconds <= kBuckets[i]) {
				buckets_[i].fetch_add(1, std::memory_order_relaxed);
			}
		}
	}
	[[nodiscard]] double sum() const noexcept { return sum_.load(std::memory_order_relaxed); }
	[[nodiscard]] std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
	[[nodiscard]] std::uint64_t bucket(
		std::size_t i) const noexcept {
		return buckets_.at(i).load(std::memory_order_relaxed);
	}

private:
	std::array<std::atomic<std::uint64_t>, 11> buckets_{};
	std::atomic<double> sum_{};
	std::atomic<std::uint64_t> count_{};
};
// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

// Method bucket: maps HTTP method std::string to an index 0-7.
namespace metrics_detail {

constexpr std::size_t N_METHODS = 8;
constexpr std::size_t N_STATUS = 6; // 1xx 2xx 3xx 4xx 5xx other

constexpr std::array<std::string_view, N_METHODS> kMethodNames =
	{"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS", "OTHER"};
std::size_t method_idx(
	std::string_view m) {
	auto it = std::ranges::find(kMethodNames, m);
	return it != kMethodNames.end() ? static_cast<std::size_t>(it - kMethodNames.begin()) : N_METHODS - 1;
}
std::size_t status_idx(
	int s) {
	std::size_t const klass = (s >= 100 && s < 600) ? static_cast<std::size_t>(s / 100) - 1 : 5;
	return klass < 5 ? klass : 5;
}
constexpr std::array<std::string_view, N_STATUS> kStatusLabels = {"1xx", "2xx", "3xx", "4xx", "5xx", "other"};

} // namespace metrics_detail

struct PrometheusLabel {
	std::string_view name;
	std::string_view value;
};

void append_prometheus_label_set(
	std::string &out,
	std::initializer_list<PrometheusLabel> labels) {
	if (labels.size() == 0) {
		return;
	}
	out += '{';
	bool first = true;
	for (auto const &label: labels) {
		if (!first) {
			out += ',';
		}
		first = false;
		out += label.name;
		out += R"(=")";
		append_json_string_content_fallback(out, label.value);
		out += '"';
	}
	out += '}';
}

void append_prometheus_sample_prefix(
	std::string &out,
	std::string_view metric,
	std::initializer_list<PrometheusLabel> labels = {}) {
	out += metric;
	append_prometheus_label_set(out, labels);
}

void append_prometheus_sample(
	std::string &out,
	std::string_view metric,
	std::initializer_list<PrometheusLabel> labels,
	std::uint64_t value) {
	append_prometheus_sample_prefix(out, metric, labels);
	out += std::format(" {}\n", value);
}

void append_prometheus_sample(
	std::string &out,
	std::string_view metric,
	std::initializer_list<PrometheusLabel> labels,
	double value) {
	append_prometheus_sample_prefix(out, metric, labels);
	out += std::format(" {}\n", value);
}

std::string format_pressure_metrics_prometheus(
	conflux::http::HttpPressureMetrics const &pressure) {
	std::string out;
	out.reserve(1024);
	out += "# HELP http_pressure_events_total HTTP lifecycle and backpressure events\n";
	out += "# TYPE http_pressure_events_total counter\n";
	auto append = [&out](std::string_view name, std::uint64_t value) {
		append_prometheus_sample(
			out,
			"http_pressure_events_total",
			{
				{"event", name}
        },
			value);
	};
	append("accept_rejected", pressure.accept_rejected);
	append("connections_closed_for_pressure", pressure.connections_closed_for_pressure);
	append("response_backpressure_events", pressure.response_backpressure_events);
	append("sse_dropped_newest", pressure.sse_dropped_newest);
	append("sse_dropped_oldest", pressure.sse_dropped_oldest);
	append("sse_disconnected_for_pressure", pressure.sse_disconnected_for_pressure);
	append("websocket_closed_for_pressure", pressure.websocket_closed_for_pressure);
	append("drain_started", pressure.drain_started);
	append("drain_deadline_hit", pressure.drain_deadline_hit);
	append("drain_forced_close", pressure.drain_forced_close);
	return out;
}

class MetricsRegistry {
public:
	// Record one completed request.
	void record(
		std::string_view method,
		int status,
		std::chrono::steady_clock::duration elapsed) noexcept {
		auto const mi = metrics_detail::method_idx(method);
		auto const si = metrics_detail::status_idx(status);
		// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		requests_[mi][si].fetch_add(1, std::memory_order_relaxed);
		double const secs = std::chrono::duration<double>(elapsed).count();
		duration_.observe(secs);
	}
	// Render Prometheus text exposition std::format (version 0.0.4).
	[[nodiscard]] std::string format_prometheus() const {
		std::string out;
		out.reserve(2048);

		// http_requests_total
		out += "# HELP http_requests_total Total HTTP requests processed\n";
		out += "# TYPE http_requests_total counter\n";
		for (std::size_t mi = 0; mi < metrics_detail::N_METHODS; ++mi) {
			for (std::size_t si = 0; si < metrics_detail::N_STATUS; ++si) {
				// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
				auto const v = requests_[mi][si].load(std::memory_order_relaxed);
				if (v == 0) {
					continue;
				}
				append_prometheus_sample(
					out,
					"http_requests_total",
					{
						{"method",  metrics_detail::kMethodNames[mi]},
						{"status", metrics_detail::kStatusLabels[si]}
                },
					v); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			}
		}

		// http_request_duration_seconds
		out += "# HELP http_request_duration_seconds HTTP request latency\n";
		out += "# TYPE http_request_duration_seconds histogram\n";
		for (std::size_t i = 0; i < Histogram::kBuckets.size(); ++i) {
			auto const le = std::format("{}", Histogram::kBuckets[i]);
			append_prometheus_sample(
				out,
				"http_request_duration_seconds_bucket",
				{
					{"le", le}
            },
				duration_.bucket(i)); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		}
		append_prometheus_sample(
			out,
			"http_request_duration_seconds_bucket",
			{
				{"le", "+Inf"}
        },
			duration_.count());
		append_prometheus_sample(out, "http_request_duration_seconds_sum", {}, duration_.sum());
		append_prometheus_sample(out, "http_request_duration_seconds_count", {}, duration_.count());

		return out;
	}
	// Direct access to sub-metrics for custom instrumentation.
	[[nodiscard]] Histogram const &duration() const noexcept { return duration_; }

private:
	// [method][status_class] request counters
	std::array<std::array<std::atomic<std::uint64_t>, metrics_detail::N_STATUS>, metrics_detail::N_METHODS> requests_{};
	Histogram duration_{};
};
// ---------------------------------------------------------------------------
// Middleware + handler
// ---------------------------------------------------------------------------

// Middleware: intercept every request, record method + status + latency.
Router::Middleware metrics_middleware(
	MetricsRegistry &registry) {
	return [&registry](conflux::http::RequestView const &req, conflux::http::Router::Handler const &next) -> conflux::http::Response {
		auto const start = std::chrono::steady_clock::now();
		auto resp = next(req);
		registry.record(req.method, resp.status, std::chrono::steady_clock::now() - start);
		return resp;
	};
}
// Route handler: return the Prometheus metrics page.
// Usage: router.get("/metrics", metrics_handler(registry));
// WARNING: the plain handler is unauthenticated — never expose on a public
// listener; prefer metrics_handler_protected or a network-level ACL.
Router::Handler metrics_handler(
	MetricsRegistry const &registry) {
	return [&registry](conflux::http::RequestView const &) -> conflux::http::Response {
		return conflux::http::Response::prometheus(registry.format_prometheus());
	};
}
// Route handler wrapped with the supplied middleware chain (e.g. bearer_auth).
// Each middleware is applied in order: chain[0] runs first, chain.back() last.
Router::Handler metrics_handler_protected(
	MetricsRegistry const &registry,
	std::vector<conflux::http::Router::Middleware> chain) {
	Router::Handler current = metrics_handler(registry);
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		conflux::http::Router::Middleware mw = std::move(*it);
		conflux::http::Router::Handler next = std::move(current);
		current = [mw = std::move(mw), next = std::move(next)](conflux::http::RequestView const &req) -> conflux::http::Response {
			return mw(req, next);
		};
	}
	return current;
}

} // namespace conflux::http
