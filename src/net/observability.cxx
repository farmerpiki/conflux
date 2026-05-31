module;
#include <cctype>

export module conflux.net.observability;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http.json_string;
import conflux.net.router;
import conflux.net.http.response;
import conflux.net.request_id;
import conflux.net.tracing;
import conflux.utils;
import conflux.work;
#if CONFLUX_HAS_METRICS
import conflux.net.metrics;
#endif

export namespace conflux::http {

struct ObservabilityOptions {
	std::string service_name = "conflux";

	bool request_id = true;
	bool trace_context = true;
	bool access_log = true;
	bool redact_sensitive_headers = true;

	bool route_latency = true;
	bool route_request_count = true;
	bool rejection_metrics = true;

	bool pressure_metrics = true;
	bool work_pool_metrics = true;
	bool task_allocation_metrics = false;
	bool json_arena_metrics = false;

	bool register_metrics_route = true;
	std::string metrics_path = "/metrics";
	std::vector<double> latency_buckets_seconds = {};

	bool log_request_headers = false;
	bool log_response_headers = false;
	bool log_query_string = false;

	std::vector<std::string> extra_sensitive_headers = {};
	std::function<void(std::string const &)> access_log_sink = {};
	std::function<HttpPressureMetrics()> pressure_metrics_source = {};
	std::vector<std::pair<std::string, std::shared_ptr<WorkPool>>> work_pools = {};
};

struct JsonArenaMetrics {
	std::uint64_t slabs_total{};
	std::uint64_t high_water_bytes{};
	std::uint64_t allocated_bytes{};
};

struct ObservabilitySinks {
	std::function<void(std::string const &)> access_logs = {};
	std::function<HttpPressureMetrics()> pressure_metrics = {};
	std::function<JsonArenaMetrics()> json_arena_metrics = {};
};

namespace observability_detail {

constexpr std::string_view kUnmatchedRoute = "<unmatched>";
constexpr std::string_view kRedacted = "<redacted>";
constexpr std::string_view kRoutePatternParam = "__conflux_route_pattern";

[[nodiscard]] std::string status_class(
	int status) {
	if (status >= 100 && status < 600) {
		return std::format("{}xx", status / 100);
	}
	return "other";
}

[[nodiscard]] std::string upper_method(
	std::string_view method) {
	return ascii_upper(method);
}

[[nodiscard]] std::string path_without_query(
	conflux::http::RequestView const &req,
	bool include_query) {
	std::string_view path{req.path};
	if (!include_query) {
		path = conflux::http::path_without_query(path);
	}
	return std::string{path};
}

[[nodiscard]] std::string route_label(
	conflux::http::RequestView const &req,
	conflux::http::Response const &resp) {
	if (auto route = resp.headers.get("__conflux-route-pattern"); route && !route->empty()) {
		return std::string{*route};
	}
	if (auto route = req.params.get(kRoutePatternParam); route && !route->empty()) {
		return std::string{*route};
	}
	if (resp.status == kHttpNotFound) {
		return std::string{kUnmatchedRoute};
	}
	return std::string{kUnmatchedRoute};
}

[[nodiscard]] std::vector<std::string> sensitive_headers(
	ObservabilityOptions const &opts) {
	std::vector<std::string> out{
		"authorization",
		"proxy-authorization",
		"cookie",
		"set-cookie",
		"x-api-key",
		"x-api-token",
		"x-auth-token",
		"x-csrf-token"};
	for (auto header: opts.extra_sensitive_headers) {
		ascii_lower_inplace(header);
		out.push_back(std::move(header));
	}
	return out;
}

[[nodiscard]] bool is_sensitive(
	std::vector<std::string> const &headers,
	std::string_view name) {
	return std::ranges::contains(headers, ascii_lower(name));
}

void append_headers_json(
	std::string &out,
	std::string_view key,
	HttpFieldsView const &headers,
	std::vector<std::string> const &sensitive,
	bool redact) {
	out += std::format(R"(,"{}":{{)", key);
	bool first = true;
	for (auto const &[name, value]: headers) {
		if (!first) {
			out += ',';
		}
		first = false;
		auto const logged = (redact && is_sensitive(sensitive, name)) ? kRedacted : value;
		out += std::format(R"({}:{})", detail::json_string(name), detail::json_string(logged));
	}
	out += '}';
}

[[nodiscard]] std::string access_log_line(
	ObservabilityOptions const &opts,
	std::vector<std::string> const &sensitive,
	conflux::http::RequestView const &req,
	conflux::http::Response const &resp,
	std::chrono::steady_clock::duration elapsed) {
	auto const route = route_label(req, resp);
	auto const request_id = req.header("x-request-id");
	auto const traceparent = req.header("traceparent");
	auto const ms = std::chrono::duration<double, std::milli>(elapsed).count();
	std::string line = std::format(
		R"({{"event":"http_request","service":{},"request_id":{},"trace_id":{},"method":{},"path":{},"route":{},"status":{},"status_class":{},"duration_ms":{},"bytes_in":{},"bytes_out":{},"remote_addr":{},"user_agent":{},"rejection_reason":null}})",
		detail::json_string(opts.service_name),
		detail::json_string(request_id),
		detail::json_string(traceparent),
		detail::json_string(upper_method(req.method)),
		detail::json_string(path_without_query(req, opts.log_query_string)),
		detail::json_string(route),
		resp.status,
		detail::json_string(status_class(resp.status)),
		ms,
		req.body.size(),
		resp.content_length(),
		detail::json_string(req.remote_addr),
		detail::json_string(req.header("user-agent")));
	line.pop_back();
	if (opts.log_request_headers) {
		append_headers_json(line, "request_headers", req.headers, sensitive, opts.redact_sensitive_headers);
	}
	if (opts.log_response_headers) {
		HttpFieldsView response_headers;
		for (auto const &[name, value]: resp.headers) {
			if (ascii_iequals(name, "__conflux-route-pattern")) {
				continue;
			}
			response_headers.emplace_back(name, value);
		}
		append_headers_json(line, "response_headers", response_headers, sensitive, opts.redact_sensitive_headers);
	}
	line += '}';
	return line;
}

#if CONFLUX_HAS_METRICS
void append_pressure_metrics(
	std::string &out,
	HttpPressureMetrics const &pressure) {
	out += "# HELP http_pressure_connections_active Active HTTP connections under pressure accounting\n";
	out += "# TYPE http_pressure_connections_active gauge\n";
	append_prometheus_sample(out, "http_pressure_connections_active", {}, std::uint64_t{0});
	out += "# HELP http_pressure_requests_inflight In-flight HTTP requests under pressure accounting\n";
	out += "# TYPE http_pressure_requests_inflight gauge\n";
	append_prometheus_sample(out, "http_pressure_requests_inflight", {}, std::uint64_t{0});
	out += "# HELP http_pressure_response_backlog conflux::http::Response backlog under pressure accounting\n";
	out += "# TYPE http_pressure_response_backlog gauge\n";
	append_prometheus_sample(out, "http_pressure_response_backlog", {}, pressure.response_backpressure_events);
	out += "# HELP http_pressure_slow_clients Slow clients under pressure accounting\n";
	out += "# TYPE http_pressure_slow_clients gauge\n";
	append_prometheus_sample(out, "http_pressure_slow_clients", {}, pressure.drain_forced_close);
	out += "# HELP http_pressure_overflow_total HTTP pressure overflow events\n";
	out += "# TYPE http_pressure_overflow_total counter\n";
	append_prometheus_sample(
		out,
		"http_pressure_overflow_total",
		{
			{  "kind", "accept"},
			{"policy", "reject"}
    },
		pressure.accept_rejected);
	append_prometheus_sample(
		out,
		"http_pressure_overflow_total",
		{
			{  "kind", "connection"},
			{"policy",      "close"}
    },
		pressure.connections_closed_for_pressure);
	append_prometheus_sample(
		out,
		"http_pressure_overflow_total",
		{
			{  "kind",         "sse"},
			{"policy", "drop_newest"}
    },
		pressure.sse_dropped_newest);
	append_prometheus_sample(
		out,
		"http_pressure_overflow_total",
		{
			{  "kind",         "sse"},
			{"policy", "drop_oldest"}
    },
		pressure.sse_dropped_oldest);
	append_prometheus_sample(
		out,
		"http_pressure_overflow_total",
		{
			{  "kind",        "sse"},
			{"policy", "disconnect"}
    },
		pressure.sse_disconnected_for_pressure);
	append_prometheus_sample(
		out,
		"http_pressure_overflow_total",
		{
			{  "kind", "websocket"},
			{"policy",     "close"}
    },
		pressure.websocket_closed_for_pressure);
}

[[nodiscard]] std::uint64_t saturating_sub(
	std::uint64_t value,
	std::uint64_t subtrahend) noexcept {
	return value > subtrahend ? value - subtrahend : 0;
}

void append_work_pool_metrics(
	std::string &out,
	std::vector<std::pair<std::string, std::shared_ptr<WorkPool>>> const &work_pools) {
	if (work_pools.empty()) {
		return;
	}
	out += "# HELP work_pool_queue_depth Approximate accepted work items not yet completed\n";
	out += "# TYPE work_pool_queue_depth gauge\n";
	out += "# HELP work_pool_running Running workers, reported when available\n";
	out += "# TYPE work_pool_running gauge\n";
	out += "# HELP work_pool_rejected_total Work-pool enqueue rejections\n";
	out += "# TYPE work_pool_rejected_total counter\n";
	out += "# HELP work_pool_completed_total Work-pool completed jobs\n";
	out += "# TYPE work_pool_completed_total counter\n";
	for (auto const &[name, pool]: work_pools) {
		if (!pool) {
			continue;
		}
		auto const stats = pool->queue_stats();
		auto const rejected = stats.enqueue_stopped_rejections + stats.enqueue_full_rejections;
		auto const accepted = saturating_sub(stats.enqueue_attempts, rejected);
		auto const pending = saturating_sub(accepted, stats.jobs_run);
		append_prometheus_sample(
			out,
			"work_pool_queue_depth",
			{
				{"pool", name}
        },
			pending);
		append_prometheus_sample(
			out,
			"work_pool_running",
			{
				{"pool", name}
        },
			std::uint64_t{0});
		append_prometheus_sample(
			out,
			"work_pool_rejected_total",
			{
				{  "pool",      name},
				{"reason", "stopped"}
        },
			stats.enqueue_stopped_rejections);
		append_prometheus_sample(
			out,
			"work_pool_rejected_total",
			{
				{  "pool",   name},
				{"reason", "full"}
        },
			stats.enqueue_full_rejections);
		append_prometheus_sample(
			out,
			"work_pool_completed_total",
			{
				{"pool", name}
        },
			stats.jobs_run);
	}
}

void append_task_allocation_metrics(
	std::string &out) {
	auto const stats = conflux::work::root::task_allocation_stats();
	out += "# HELP work_task_frame_allocations_total Task coroutine frame allocation calls\n";
	out += "# TYPE work_task_frame_allocations_total counter\n";
	append_prometheus_sample(out, "work_task_frame_allocations_total", {}, stats.coroutine_frame_allocations);
	out += "# HELP work_task_frame_pool_hits_total Task frame pool hits\n";
	out += "# TYPE work_task_frame_pool_hits_total counter\n";
	append_prometheus_sample(out, "work_task_frame_pool_hits_total", {}, std::uint64_t{0});
	out += "# HELP work_task_frame_pool_misses_total Task frame pool misses\n";
	out += "# TYPE work_task_frame_pool_misses_total counter\n";
	append_prometheus_sample(out, "work_task_frame_pool_misses_total", {}, stats.coroutine_frame_allocations);
}

void append_json_arena_metrics(
	std::string &out,
	JsonArenaMetrics const &metrics) {
	out += "# HELP json_arena_slabs_total JSON arena slabs\n";
	out += "# TYPE json_arena_slabs_total gauge\n";
	append_prometheus_sample(out, "json_arena_slabs_total", {}, metrics.slabs_total);
	out += "# HELP json_arena_high_water_bytes JSON arena high-water bytes\n";
	out += "# TYPE json_arena_high_water_bytes gauge\n";
	append_prometheus_sample(out, "json_arena_high_water_bytes", {}, metrics.high_water_bytes);
	out += "# HELP json_arena_allocated_bytes JSON arena currently allocated bytes\n";
	out += "# TYPE json_arena_allocated_bytes gauge\n";
	append_prometheus_sample(out, "json_arena_allocated_bytes", {}, metrics.allocated_bytes);
}

struct ObservabilityRegistry {
	struct RequestKey {
		std::string service;
		std::string route;
		std::string method;
		std::string status_class;
		std::string status;

		[[nodiscard]] friend bool operator ==(RequestKey const &, RequestKey const &) = default;
	};

	struct RequestKeyHash {
		[[nodiscard]] std::size_t operator ()(
			RequestKey const &key) const noexcept {
			auto h = std::hash<std::string>{}(key.service);
			hash_combine(h, key.route);
			hash_combine(h, key.method);
			hash_combine(h, key.status_class);
			hash_combine(h, key.status);
			return h;
		}
	};

	struct DurationKey {
		std::string service;
		std::string route;
		std::string method;

		[[nodiscard]] friend bool operator ==(DurationKey const &, DurationKey const &) = default;
	};

	struct DurationKeyHash {
		[[nodiscard]] std::size_t operator ()(
			DurationKey const &key) const noexcept {
			auto h = std::hash<std::string>{}(key.service);
			hash_combine(h, key.route);
			hash_combine(h, key.method);
			return h;
		}
	};

	struct DurationState {
		std::vector<double> buckets;
		std::vector<std::uint64_t> bucket_counts;
		double sum{};
		std::uint64_t count{};
	};

	void observe(
		ObservabilityOptions const &opts,
		conflux::http::RequestView const &req,
		conflux::http::Response const &resp,
		std::chrono::steady_clock::duration elapsed) {
		auto const route = route_label(req, resp);
		auto const method = upper_method(req.method);
		auto const status = std::to_string(resp.status);
		auto const klass = status_class(resp.status);
		auto const seconds = std::chrono::duration<double>(elapsed).count();
		std::scoped_lock const lock{mutex};
		if (opts.route_request_count) {
			++requests[RequestKey{
				.service = opts.service_name,
				.route = route,
				.method = method,
				.status_class = klass,
				.status = status}];
		}
		if (opts.route_latency) {
			auto &duration = durations[DurationKey{.service = opts.service_name, .route = route, .method = method}];
			if (duration.buckets.empty()) {
				duration.buckets = opts.latency_buckets_seconds.empty() ?
									   std::vector<double>{Histogram::kBuckets.begin(), Histogram::kBuckets.end()} :
									   opts.latency_buckets_seconds;
				std::ranges::sort(duration.buckets);
				duration.bucket_counts.assign(duration.buckets.size(), 0);
			}
			duration.sum += seconds;
			++duration.count;
			for (std::size_t i = 0; i < duration.buckets.size(); ++i) {
				if (seconds <= duration.buckets[i]) {
					++duration.bucket_counts[i];
				}
			}
		}
		if (opts.rejection_metrics && resp.status >= 400) {
			++rejections[std::pair{std::format("status_{}", klass), status}];
		}
	}

	void observe_server_rejection(
		HttpRejectReason reason,
		int status) {
		std::scoped_lock const lock{mutex};
		++rejections[std::pair{std::string{reject_reason_code(reason)}, std::to_string(status)}];
	}

	[[nodiscard]] std::string format_prometheus(
		ObservabilityOptions const &opts,
		ObservabilitySinks const &sinks) const {
		std::scoped_lock const lock{mutex};
		std::string out;
		out.reserve(4096);
		out += "# HELP http_requests_total Total HTTP requests processed\n";
		out += "# TYPE http_requests_total counter\n";
		for (auto const &[key, value]: requests) {
			append_prometheus_sample(
				out,
				"http_requests_total",
				{
					{     "service",      key.service},
					{       "route",        key.route},
					{      "method",       key.method},
					{"status_class", key.status_class},
					{      "status",       key.status}
            },
				value);
		}
		out += "# HELP http_request_duration_seconds HTTP request latency\n";
		out += "# TYPE http_request_duration_seconds histogram\n";
		for (auto const &[key, duration]: durations) {
			for (std::size_t i = 0; i < duration.buckets.size(); ++i) {
				auto const le = std::format("{}", duration.buckets[i]);
				append_prometheus_sample(
					out,
					"http_request_duration_seconds_bucket",
					{
						{"service", key.service},
						{  "route",   key.route},
						{ "method",  key.method},
						{     "le",          le}
                },
					duration.bucket_counts[i]);
			}
			append_prometheus_sample(
				out,
				"http_request_duration_seconds_bucket",
				{
					{"service", key.service},
					{  "route",   key.route},
					{ "method",  key.method},
					{     "le",      "+Inf"}
            },
				duration.count);
			append_prometheus_sample(
				out,
				"http_request_duration_seconds_sum",
				{
					{"service", key.service},
					{  "route",   key.route},
					{ "method",  key.method}
            },
				duration.sum);
			append_prometheus_sample(
				out,
				"http_request_duration_seconds_count",
				{
					{"service", key.service},
					{  "route",   key.route},
					{ "method",  key.method}
            },
				duration.count);
		}
		out += "# HELP http_rejections_total HTTP rejected/problem responses\n";
		out += "# TYPE http_rejections_total counter\n";
		for (auto const &[key, value]: rejections) {
			append_prometheus_sample(
				out,
				"http_rejections_total",
				{
					{"service", opts.service_name},
					{ "reason",         key.first},
					{ "status",        key.second}
            },
				value);
		}
		if (opts.pressure_metrics) {
			if (sinks.pressure_metrics) {
				append_pressure_metrics(out, sinks.pressure_metrics());
			} else if (opts.pressure_metrics_source) {
				append_pressure_metrics(out, opts.pressure_metrics_source());
			}
		}
		if (opts.work_pool_metrics) {
			append_work_pool_metrics(out, opts.work_pools);
		}
		if (opts.task_allocation_metrics) {
			append_task_allocation_metrics(out);
		}
		if (opts.json_arena_metrics && sinks.json_arena_metrics) {
			append_json_arena_metrics(out, sinks.json_arena_metrics());
		}
		return out;
	}

	mutable std::mutex mutex;
	std::unordered_map<RequestKey, std::uint64_t, RequestKeyHash> requests;
	std::unordered_map<DurationKey, DurationState, DurationKeyHash> durations;
	std::map<std::pair<std::string, std::string>, std::uint64_t> rejections;
};
#endif

struct ObservabilityState {
	ObservabilityOptions options;
	ObservabilitySinks sinks;
	std::vector<std::string> sensitive;
#if CONFLUX_HAS_METRICS
	std::shared_ptr<ObservabilityRegistry> registry;
#endif
};

} // namespace observability_detail

struct ObservabilityMiddleware {
	ObservabilityOptions options;
	std::shared_ptr<observability_detail::ObservabilityState> state;

	[[nodiscard]] conflux::http::Response operator ()(
		conflux::http::RequestView const &req,
		conflux::http::Router::Handler const &next) const {
		auto observed_req = req.to_owned();
		observed_req.params.set("__conflux_observe_route", "1");
		conflux::http::RequestView const observed_view{observed_req};
		auto start = std::chrono::steady_clock::now();
		auto resp = next(observed_view);
		auto elapsed = std::chrono::steady_clock::now() - start;
#if CONFLUX_HAS_METRICS
		if (state && state->registry) {
			state->registry->observe(state->options, observed_view, resp, elapsed);
		}
#endif
		if (state && state->options.access_log) {
			auto line =
				observability_detail::access_log_line(state->options, state->sensitive, observed_view, resp, elapsed);
			if (state->sinks.access_logs) {
				state->sinks.access_logs(line);
			} else if (state->options.access_log_sink) {
				state->options.access_log_sink(line);
			} else {
				eprintln(line);
			}
		}
		resp.headers.erase("__conflux-route-pattern");
		return resp;
	}
};

[[nodiscard]] ObservabilityMiddleware observability(
	ObservabilityOptions options = {}) {
	auto state = std::make_shared<observability_detail::ObservabilityState>();
	state->options = options;
	state->sinks.access_logs = options.access_log_sink;
	state->sinks.pressure_metrics = options.pressure_metrics_source;
	state->sensitive = observability_detail::sensitive_headers(options);
#if CONFLUX_HAS_METRICS
	state->registry = std::make_shared<observability_detail::ObservabilityRegistry>();
#endif
	return ObservabilityMiddleware{.options = std::move(options), .state = std::move(state)};
}

[[nodiscard]] ObservabilityMiddleware observability(
	ObservabilityOptions options,
	ObservabilitySinks sinks) {
	auto state = std::make_shared<observability_detail::ObservabilityState>();
	state->options = options;
	state->sinks = std::move(sinks);
	state->sensitive = observability_detail::sensitive_headers(options);
#if CONFLUX_HAS_METRICS
	state->registry = std::make_shared<observability_detail::ObservabilityRegistry>();
#endif
	return ObservabilityMiddleware{.options = std::move(options), .state = std::move(state)};
}

[[nodiscard]] HttpServerObservabilityHooks observability_server_hooks(
	ObservabilityMiddleware const &middleware) {
	return HttpServerObservabilityHooks{
		.rejection =
			[state = middleware.state](HttpRejectReason reason, int status) {
#if !CONFLUX_HAS_METRICS
				static_cast<void>(reason);
				static_cast<void>(status);
#endif
				if (state && state->options.rejection_metrics) {
#if CONFLUX_HAS_METRICS
					if (state->registry) {
						state->registry->observe_server_rejection(reason, status);
					}
#endif
				}
			},
	};
}

#if CONFLUX_HAS_METRICS
[[nodiscard]] conflux::http::Router::Handler observability_metrics_handler(
	ObservabilityMiddleware const &middleware) {
	return [state = middleware.state](conflux::http::RequestView const &) -> conflux::http::Response {
		return conflux::http::Response::prometheus(
			state && state->registry ? state->registry->format_prometheus(state->options, state->sinks) :
									   std::string{});
	};
}
#endif

} // namespace conflux::http
