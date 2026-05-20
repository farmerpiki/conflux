module;
#include <cctype>
#include <cstdio>

export module conflux.net.observability;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.router;
import conflux.net.request_id;
import conflux.net.tracing;
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
	std::string out{method};
	std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
	return out;
}

[[nodiscard]] std::string path_without_query(
	HttpRequestView const &req,
	bool include_query) {
	std::string_view path{req.path};
	if (!include_query) {
		if (auto q = path.find('?'); q != std::string_view::npos) {
			path = path.substr(0, q);
		}
	}
	return std::string{path};
}

[[nodiscard]] std::string route_label(
	HttpRequestView const &req,
	HttpResponse const &resp) {
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

[[nodiscard]] std::string json_escape(
	std::string_view s) {
	std::string out;
	out.reserve(s.size() + 4);
	for (char const raw: s) {
		auto c = static_cast<unsigned char>(raw);
		if (c == '"') {
			out += "\\\"";
		} else if (c == '\\') {
			out += "\\\\";
		} else if (c == '\n') {
			out += "\\n";
		} else if (c == '\r') {
			out += "\\r";
		} else if (c == '\t') {
			out += "\\t";
		} else if (c < 0x20) {
			out += std::format("\\u{:04x}", c);
		} else {
			out += static_cast<char>(c);
		}
	}
	return out;
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
		std::ranges::transform(header, header.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		out.push_back(std::move(header));
	}
	return out;
}

[[nodiscard]] bool is_sensitive(
	std::vector<std::string> const &headers,
	std::string_view name) {
	std::string lower{name};
	std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return std::ranges::contains(headers, lower);
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
		out += std::format(R"("{}":"{}")", json_escape(name), json_escape(logged));
	}
	out += '}';
}

[[nodiscard]] std::string access_log_line(
	ObservabilityOptions const &opts,
	std::vector<std::string> const &sensitive,
	HttpRequestView const &req,
	HttpResponse const &resp,
	std::chrono::steady_clock::duration elapsed) {
	auto const route = route_label(req, resp);
	auto const request_id = req.header("x-request-id");
	auto const traceparent = req.header("traceparent");
	auto const ms = std::chrono::duration<double, std::milli>(elapsed).count();
	std::string line = std::format(
		R"({{"event":"http_request","service":"{}","request_id":"{}","trace_id":"{}","method":"{}","path":"{}","route":"{}","status":{},"status_class":"{}","duration_ms":{},"bytes_in":{},"bytes_out":{},"remote_addr":"{}","user_agent":"{}","rejection_reason":null}})",
		json_escape(opts.service_name),
		json_escape(request_id),
		json_escape(traceparent),
		json_escape(upper_method(req.method)),
		json_escape(path_without_query(req, opts.log_query_string)),
		json_escape(route),
		resp.status,
		status_class(resp.status),
		ms,
		req.body.size(),
		resp.content_length(),
		json_escape(req.remote_addr),
		json_escape(req.header("user-agent")));
	line.pop_back();
	if (opts.log_request_headers) {
		append_headers_json(line, "request_headers", req.headers, sensitive, opts.redact_sensitive_headers);
	}
	if (opts.log_response_headers) {
		HttpFieldsView response_headers;
		for (auto const &[name, value]: resp.headers) {
			response_headers.emplace_back(name, value);
		}
		append_headers_json(line, "response_headers", response_headers, sensitive, opts.redact_sensitive_headers);
	}
	line += '}';
	return line;
}

#if CONFLUX_HAS_METRICS
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
			auto mix = [&h](std::string const &value) {
				h ^= std::hash<std::string>{}(value) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
			};
			mix(key.route);
			mix(key.method);
			mix(key.status_class);
			mix(key.status);
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
			auto mix = [&h](std::string const &value) {
				h ^= std::hash<std::string>{}(value) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
			};
			mix(key.route);
			mix(key.method);
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
		HttpRequestView const &req,
		HttpResponse const &resp,
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
			++rejections[std::pair{klass, status}];
		}
	}

	[[nodiscard]] std::string format_prometheus(
		ObservabilityOptions const &opts) const {
		std::scoped_lock const lock{mutex};
		std::string out;
		out.reserve(4096);
		out += "# HELP http_requests_total Total HTTP requests processed\n";
		out += "# TYPE http_requests_total counter\n";
		for (auto const &[key, value]: requests) {
			out += std::format(
				R"(http_requests_total{{service="{}",route="{}",method="{}",status_class="{}",status="{}"}} {})"
				"\n",
				key.service,
				key.route,
				key.method,
				key.status_class,
				key.status,
				value);
		}
		out += "# HELP http_request_duration_seconds HTTP request latency\n";
		out += "# TYPE http_request_duration_seconds histogram\n";
		for (auto const &[key, duration]: durations) {
			for (std::size_t i = 0; i < duration.buckets.size(); ++i) {
				out += std::format(
					R"(http_request_duration_seconds_bucket{{service="{}",route="{}",method="{}",le="{}"}} {})"
					"\n",
					key.service,
					key.route,
					key.method,
					duration.buckets[i],
					duration.bucket_counts[i]);
			}
			out += std::format(
				R"(http_request_duration_seconds_bucket{{service="{}",route="{}",method="{}",le="+Inf"}} {})"
				"\n",
				key.service,
				key.route,
				key.method,
				duration.count);
			out += std::format(
				R"(http_request_duration_seconds_sum{{service="{}",route="{}",method="{}"}} {})"
				"\n",
				key.service,
				key.route,
				key.method,
				duration.sum);
			out += std::format(
				R"(http_request_duration_seconds_count{{service="{}",route="{}",method="{}"}} {})"
				"\n",
				key.service,
				key.route,
				key.method,
				duration.count);
		}
		out += "# HELP http_rejections_total HTTP rejected/problem responses\n";
		out += "# TYPE http_rejections_total counter\n";
		for (auto const &[key, value]: rejections) {
			out += std::format(
				R"(http_rejections_total{{service="{}",reason="status_{}",status="{}"}} {})"
				"\n",
				opts.service_name,
				key.first,
				key.second,
				value);
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
	std::vector<std::string> sensitive;
#if CONFLUX_HAS_METRICS
	std::shared_ptr<ObservabilityRegistry> registry;
#endif
};

} // namespace observability_detail

struct ObservabilityMiddleware {
	ObservabilityOptions options;
	std::shared_ptr<observability_detail::ObservabilityState> state;

	[[nodiscard]] HttpResponse operator ()(
		HttpRequestView const &req,
		Router::Handler const &next) const {
		auto observed_req = req.to_owned();
		observed_req.params.set("__conflux_observe_route", "1");
		HttpRequestView const observed_view{observed_req};
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
			if (state->options.access_log_sink) {
				state->options.access_log_sink(line);
			} else {
				std::println(stderr, "{}", line);
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
	state->sensitive = observability_detail::sensitive_headers(options);
#if CONFLUX_HAS_METRICS
	state->registry = std::make_shared<observability_detail::ObservabilityRegistry>();
#endif
	return ObservabilityMiddleware{.options = std::move(options), .state = std::move(state)};
}

#if CONFLUX_HAS_METRICS
[[nodiscard]] Router::Handler observability_metrics_handler(
	ObservabilityMiddleware const &middleware) {
	return [state = middleware.state](HttpRequestView const &) -> HttpResponse {
		HttpResponse r;
		r.status = kHttpOk;
		r.status_text = "OK";
		r.content_type = "text/plain; version=0.0.4; charset=utf-8";
		r.set_text_body(state && state->registry ? state->registry->format_prometheus(state->options) : std::string{});
		return r;
	};
}
#endif

} // namespace conflux::http
