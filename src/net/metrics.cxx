module;
#include <cstdint>

export module conflux.net.metrics;
import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

// Atomic counter (monotonically increasing, thread-safe).
export class Counter {
	Atom<u64> value_;

public:
	void inc() noexcept { inc(1); }
	void inc(
		u64 n) noexcept {
		value_.fetch_add(n, memory_order_relaxed);
	}
	[[nodiscard]] u64 get() const noexcept { return value_.load(memory_order_relaxed); }
};
// Gauge (current value, can go up or down).
export class Gauge {
	Atom<double> value_;

public:
	void set(
		double v) noexcept {
		value_.store(v, memory_order_relaxed);
	}
	void inc(
		double n) noexcept {
		value_.fetch_add(n, memory_order_relaxed);
	}
	void dec(
		double n) noexcept {
		value_.fetch_sub(n, memory_order_relaxed);
	}
	[[nodiscard]] double get() const noexcept { return value_.load(memory_order_relaxed); }
};
// Fixed-bucket histogram for latency measurements.
// Buckets are upper bounds in seconds (standard Prometheus latency buckets).
export class Histogram {
public:
	static constexpr A<double, 11> kBuckets = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0};
	void observe(
		double seconds) noexcept {
		sum_.fetch_add(seconds, memory_order_relaxed);
		count_.fetch_add(1, memory_order_relaxed);
		for (auto [bound, cnt]: views::zip(kBuckets, buckets_)) {
			if (seconds <= bound) {
				cnt.fetch_add(1, memory_order_relaxed);
			}
		}
	}
	[[nodiscard]] double sum() const noexcept { return sum_.load(memory_order_relaxed); }
	[[nodiscard]] u64 count() const noexcept { return count_.load(memory_order_relaxed); }
	[[nodiscard]] u64 bucket(
		SZ i) const noexcept {
		return buckets_.at(i).load(memory_order_relaxed);
	}

private:
	A<Atom<u64>, 11> buckets_{};
	Atom<double> sum_{};
	Atom<u64> count_{};
};
// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

// Method bucket: maps HTTP method S to an index 0-7.
namespace {

constexpr SZ N_METHODS = 8;
constexpr SZ N_STATUS = 6; // 1xx 2xx 3xx 4xx 5xx other

constexpr A<SV, N_METHODS> kMethodNames = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS", "OTHER"};
SZ method_idx(
	SV m) {
	auto it = ranges::find(kMethodNames, m);
	return it != kMethodNames.end() ? static_cast<SZ>(it - kMethodNames.begin()) : N_METHODS - 1;
}
SZ status_idx(
	int s) {
	SZ const klass = (s >= 100 && s < 600) ? static_cast<SZ>(s / 100) - 1 : 5;
	return klass < 5 ? klass : 5;
}
constexpr A<SV, N_STATUS> kStatusLabels = {"1xx", "2xx", "3xx", "4xx", "5xx", "other"};

} // namespace
export class MetricsRegistry {
public:
	// Record one completed request.
	void record(
		SV method,
		int status,
		chrono::steady_clock::duration elapsed) noexcept {
		auto const mi = method_idx(method);
		auto const si = status_idx(status);
		// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		requests_[mi][si].fetch_add(1, memory_order_relaxed);
		double const secs = chrono::duration<double>(elapsed).count();
		duration_.observe(secs);
	}
	// Render Prometheus text exposition format (version 0.0.4).
	[[nodiscard]] S format_prometheus() const {
		S out;
		out.reserve(2048);

		// http_requests_total
		out += "# HELP http_requests_total Total HTTP requests processed\n";
		out += "# TYPE http_requests_total counter\n";
		for (SZ mi = 0; mi < N_METHODS; ++mi) {
			for (SZ si = 0; si < N_STATUS; ++si) {
				// NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
				auto const v = requests_[mi][si].load(memory_order_relaxed);
				if (v == 0) {
					continue;
				}
				out += format(
					"http_requests_total{{method=\"{}\",status=\"{}\"}} {}\n",
					kMethodNames[mi],
					kStatusLabels[si],
					v); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
			}
		}

		// http_request_duration_seconds
		out += "# HELP http_request_duration_seconds HTTP request latency\n";
		out += "# TYPE http_request_duration_seconds histogram\n";
		for (SZ i = 0; i < Histogram::kBuckets.size(); ++i) {
			out += format(
				"http_request_duration_seconds_bucket{{le=\"{}\"}} {}\n",
				Histogram::kBuckets[i],
				duration_.bucket(i)); // NOLINT(cppcoreguidelines-pro-bounds-constant-A-index)
		}
		out += format("http_request_duration_seconds_bucket{{le=\"+Inf\"}} {}\n", duration_.count());
		out += format("http_request_duration_seconds_sum {}\n", duration_.sum());
		out += format("http_request_duration_seconds_count {}\n", duration_.count());

		return out;
	}
	// Direct access to sub-metrics for custom instrumentation.
	[[nodiscard]] Histogram const &duration() const noexcept { return duration_; }

private:
	// [method][status_class] request counters
	A<A<Atom<u64>, N_STATUS>, N_METHODS> requests_{};
	Histogram duration_{};
};
// ---------------------------------------------------------------------------
// Middleware + handler
// ---------------------------------------------------------------------------

// Middleware: intercept every request, record method + status + latency.
export Router::Middleware metrics_middleware(
	MetricsRegistry &registry) {
	return [&registry](HttpRequestView const &req, Router::Handler const &next) -> HttpResponse {
		auto const start = chrono::steady_clock::now();
		auto resp = next(req);
		registry.record(req.method, resp.status, chrono::steady_clock::now() - start);
		return resp;
	};
}
// Route handler: return the Prometheus metrics page.
// Usage: router.get("/metrics", metrics_handler(registry));
// WARNING: the plain handler is unauthenticated — never expose on a public
// listener; prefer metrics_handler_protected or a network-level ACL.
export Router::Handler metrics_handler(
	MetricsRegistry const &registry) {
	return [&registry](HttpRequestView const &) -> HttpResponse {
		HttpResponse r;
		r.status = 200;
		r.status_text = "OK";
		r.content_type = "text/plain; version=0.0.4; charset=utf-8";
		r.set_text_body(registry.format_prometheus());
		return r;
	};
}
// Route handler wrapped with the supplied middleware chain (e.g. bearer_auth).
// Each middleware is applied in order: chain[0] runs first, chain.back() last.
export Router::Handler metrics_handler_protected(
	MetricsRegistry const &registry,
	V<Router::Middleware> chain) {
	Router::Handler current = metrics_handler(registry);
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		Router::Middleware mw = move(*it);
		Router::Handler next = move(current);
		current = [mw = move(mw), next = move(next)](HttpRequestView const &req) -> HttpResponse {
			return mw(req, next);
		};
	}
	return current;
}
