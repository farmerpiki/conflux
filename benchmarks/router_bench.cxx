import std;

import conflux.net.http;

using namespace std;

namespace benchmark_detail {

inline atomic<size_t> sink{};

struct Config {
	bool list_only = false;
	string filter;
	optional<size_t> iterations_override;
	enum class Format : uint8_t {
		table,
		csv,
	};
	Format format = Format::table;
};

struct Stats {
	string_view name;
	size_t iterations{};
	uint64_t total_ns{};
	double ns_per_iter{};
};

using BenchFn = function<size_t()>;

struct Case {
	string_view name;
	string_view description;
	size_t default_iterations;
	BenchFn run;
};

[[gnu::const]] size_t iterations_for_payload(
	size_t payload_size) {
	if (payload_size <= 512) {
		return 50'000;
	}
	if (payload_size <= 4096) {
		return 20'000;
	}
	if (payload_size <= 16'384) {
		return 5'000;
	}
	return 1'000;
}

bool force_gzip_backend(
	GzipBackend backend) {
	set_compression_calibration(CompressionCalibration::disabled);
	return set_gzip_backend(backend);
}

void print_usage() {
	println("Usage: conflux_benchmarks [--list] [--filter SUBSTR] [--iterations N] [--format table|csv]");
}

Config parse_args(
	span<char *> args) {
	Config cfg;
	for (size_t i = 1; i < args.size(); ++i) {
		string_view arg = args[i];
		if (arg == "--list") {
			cfg.list_only = true;
			continue;
		}
		if (arg == "--help" || arg == "-h") {
			print_usage();
			exit(0);
		}
		if (arg == "--filter") {
			if (i + 1 >= args.size()) {
				throw invalid_argument{"--filter requires a value"};
			}
			cfg.filter = args[++i];
			continue;
		}
		if (arg == "--iterations") {
			if (i + 1 >= args.size()) {
				throw invalid_argument{"--iterations requires a value"};
			}
			size_t iters = 0;
			auto const value = string_view{args[++i]};
			auto const [ptr, ec] = from_chars(value.data(), value.data() + value.size(), iters);
			if (ec != errc{} || ptr != value.data() + value.size() || iters == 0) {
				throw invalid_argument{"--iterations must be a positive integer"};
			}
			cfg.iterations_override = iters;
			continue;
		}
		if (arg == "--format") {
			if (i + 1 >= args.size()) {
				throw invalid_argument{"--format requires a value"};
			}
			auto const value = string_view{args[++i]};
			if (value == "table") {
				cfg.format = Config::Format::table;
			} else if (value == "csv") {
				cfg.format = Config::Format::csv;
			} else {
				throw invalid_argument{"--format must be table or csv"};
			}
			continue;
		}
		throw invalid_argument{format("unknown argument: {}", arg)};
	}
	return cfg;
}

[[gnu::pure]] bool matches_filter(
	Case const &bench,
	string_view filter) {
	return filter.empty() || bench.name.contains(filter) || bench.description.contains(filter);
}

[[gnu::const]] size_t warmup_iterations(
	size_t iterations) {
	return clamp(iterations / 10, size_t{1}, size_t{1000});
}

Stats measure_case(
	Case const &bench,
	size_t iterations) {
	for (size_t i = 0; i < warmup_iterations(iterations); ++i) {
		sink.fetch_add(bench.run(), memory_order_relaxed);
	}

	auto const start = chrono::steady_clock::now();
	for (size_t i = 0; i < iterations; ++i) {
		sink.fetch_add(bench.run(), memory_order_relaxed);
	}
	auto const elapsed = chrono::steady_clock::now() - start;
	auto const total_ns = static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(elapsed).count());
	return Stats{
		.name = bench.name,
		.iterations = iterations,
		.total_ns = total_ns,
		.ns_per_iter = static_cast<double>(total_ns) / static_cast<double>(iterations)};
}

void print_list(
	vector<Case> const &cases) {
	for (auto const &bench: cases) {
		println("{:32} {}", bench.name, bench.description);
	}
}

void print_header(
	Config::Format format) {
	if (format == Config::Format::table) {
		println("{:32} {:>12} {:>14} {:>14}", "Benchmark", "Iterations", "Total (ms)", "ns/iter");
	} else {
		println("name,iterations,total_ns,ns_per_iter");
	}
}

void print_stats(
	Stats const &stats,
	Config::Format format) {
	if (format == Config::Format::table) {
		auto const total_ms = static_cast<double>(stats.total_ns) / 1'000'000.0;
		println("{:32} {:>12} {:>14.3f} {:>14.1f}", stats.name, stats.iterations, total_ms, stats.ns_per_iter);
	} else {
		println("{},{},{},{}", stats.name, stats.iterations, stats.total_ns, stats.ns_per_iter);
	}
}

Case make_httpfields_lookup_case() {
	auto headers = make_shared<HttpFields>(true);
	headers->emplace_back("Content-Type", "application/json");
	headers->emplace_back("Accept-Encoding", "br, zstd, gzip");
	headers->emplace_back("X-Request-Id", "abc-123");
	headers->emplace_back("X-Forwarded-For", "127.0.0.1");
	return Case{
		.name = "micro/httpfields_lookup",
		.description = "Case-insensitive hot-path header lookup",
		.default_iterations = 2'000'000,
		.run = [headers] {
			return headers->operator []("accept-encoding").size()
				 + headers->operator []("content-type").size()
				 + headers->operator []("missing").size();
		}};
}

Case make_router_exact_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = make_shared<State>();
	state->router.get("/health", [](HttpRequestView const &) { return HttpResponse::text("ok"); });
	state->req.method = "GET";
	state->req.path = "/health";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "micro/router_dispatch_exact",
		.description = "Exact-path route dispatch without middleware",
		.default_iterations = 500'000,
		.run = [state] { return state->router.dispatch(state->req).text_body().size(); }};
}

Case make_router_params_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = make_shared<State>();
	state->router.get("/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::text(format("{}:{}", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "micro/router_dispatch_params",
		.description = "Parameterized route matching with two captures",
		.default_iterations = 300'000,
		.run = [state] { return state->router.dispatch(state->req).text_body().size(); }};
}

Case make_compress_case() {
	struct State {
		Router router;
		HttpRequest req;
		shared_ptr<string> payload{make_shared<string>()};
	};
	auto state = make_shared<State>();
	state->payload->assign(4096, 'x');
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) { return HttpResponse::text(*payload); });
	state->req.method = "GET";
	state->req.path = "/data";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "br, zstd, gzip";
	return Case{
		.name = "micro/compress_middleware",
		.description = "Compression negotiation and body rewrite on a text response",
		.default_iterations = 20'000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}

Case make_compress_negotiation_miss_case() {
	struct State {
		Router router;
		HttpRequest req;
		shared_ptr<string> payload{make_shared<string>()};
	};
	auto state = make_shared<State>();
	state->payload->assign(4096, 'x');
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) { return HttpResponse::text(*payload); });
	state->req.method = "GET";
	state->req.path = "/data";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "identity";
	return Case{
		.name = "micro/compress_negotiation_miss",
		.description = "Compression middleware with no matching encoding",
		.default_iterations = 200'000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}

Case make_compress_below_threshold_case() {
	struct State {
		Router router;
		HttpRequest req;
		shared_ptr<string> payload{make_shared<string>()};
	};
	auto state = make_shared<State>();
	state->payload->assign(128, 'x');
	state->router.use(compress_middleware({.min_body_size = 256}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) { return HttpResponse::text(*payload); });
	state->req.method = "GET";
	state->req.path = "/data";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "br, zstd, gzip";
	return Case{
		.name = "micro/compress_below_threshold",
		.description = "Compression middleware fast path for bodies below min size",
		.default_iterations = 300'000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}

Case make_codec_payload_case_owned(
	string codec_name,
	size_t payload_size) {
	struct State {
		string name;
		string description;
		Router router;
		HttpRequest req;
		shared_ptr<string> payload{make_shared<string>()};
	};
	auto state = make_shared<State>();
	state->name = format("codec/{}/{}B", codec_name, payload_size);
	state->description = format("Compression path pinned to {} for {} byte text payloads", codec_name, payload_size);
	state->payload->assign(payload_size, 'x');
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) { return HttpResponse::text(*payload); });
	state->req.method = "GET";
	state->req.path = "/data";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = codec_name;
	return Case{
		.name = state->name,
		.description = state->description,
		.default_iterations = iterations_for_payload(payload_size),
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}

Case make_gzip_backend_payload_case(
	GzipBackend backend,
	size_t payload_size) {
	struct State {
		string name;
		string description;
		Router router;
		HttpRequest req;
		shared_ptr<string> payload{make_shared<string>()};
		GzipBackend backend;
		bool configured = false;
	};
	auto state = make_shared<State>();
	state->backend = backend;
	state->name = format("backend/{}/{}B", gzip_backend_name(backend), payload_size);
	state->description = format("Gzip backend {} on {} byte text payloads", gzip_backend_name(backend), payload_size);
	state->payload->assign(payload_size, 'x');
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) { return HttpResponse::text(*payload); });
	state->req.method = "GET";
	state->req.path = "/data";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "gzip";
	return Case{
		.name = state->name,
		.description = state->description,
		.default_iterations = iterations_for_payload(payload_size),
		.run = [state] {
			bool const ok = state->configured || force_gzip_backend(state->backend);
			if (!ok) {
				return size_t{0};
			}
			state->configured = true;
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}

Case make_route_json_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = make_shared<State>();
	state->router.get("/api/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::json(
			format(R"({{"user":"{}","post":"{}","ok":true}})", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/api/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "flow/route_json_only",
		.description = "Route params plus JSON body construction without middleware",
		.default_iterations = 200'000,
		.run = [state] { return state->router.dispatch(state->req).text_body().size(); }};
}

Case make_route_json_with_header_middleware_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = make_shared<State>();
	state->router.use([](HttpRequestView const &req, Router::Handler const &next) {
		auto resp = next(req);
		resp.headers["X-Bench"] = "flow";
		return resp;
	});
	state->router.get("/api/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::json(
			format(R"({{"user":"{}","post":"{}","ok":true}})", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/api/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "flow/route_json_with_header_mw",
		.description = "Route params and JSON construction with a lightweight middleware wrapper",
		.default_iterations = 150'000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["X-Bench"].size();
		}};
}

Case make_route_json_with_compress_negotiation_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = make_shared<State>();
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/api/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::json(
			format(R"({{"user":"{}","post":"{}","ok":true}})", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/api/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "identity";
	return Case{
		.name = "flow/route_json_with_compress_miss",
		.description = "Route params and JSON body through compression middleware with no matching codec",
		.default_iterations = 150'000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}

Case make_flow_route_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = make_shared<State>();
	state->router.use([](HttpRequestView const &req, Router::Handler const &next) {
		auto resp = next(req);
		resp.headers["X-Bench"] = "flow";
		return resp;
	});
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/api/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::json(
			format(R"({{"user":"{}","post":"{}","ok":true}})", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/api/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "br, zstd, gzip";
	return Case{
		.name = "flow/api_route_with_middleware",
		.description = "Full in-process request flow through middleware, params, and response shaping",
		.default_iterations = 100'000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["X-Bench"].size();
		}};
}

Case make_flow_not_found_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = make_shared<State>();
	state->router.use([](HttpRequestView const &req, Router::Handler const &next) { return next(req); });
	state->router.get("/api/health", [](HttpRequestView const &) { return HttpResponse::text("ok"); });
	state->req.method = "GET";
	state->req.path = "/api/missing/resource";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "flow/not_found_request",
		.description = "Miss path through the router stack",
		.default_iterations = 200'000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return static_cast<size_t>(resp.status) + resp.text_body().size();
		}};
}

vector<Case> build_cases() {
	vector<Case> cases;
	cases.push_back(make_httpfields_lookup_case());
	cases.push_back(make_router_exact_case());
	cases.push_back(make_router_params_case());
	cases.push_back(make_compress_case());
	cases.push_back(make_compress_negotiation_miss_case());
	cases.push_back(make_compress_below_threshold_case());
	cases.push_back(make_route_json_case());
	cases.push_back(make_route_json_with_header_middleware_case());
	cases.push_back(make_route_json_with_compress_negotiation_case());
	cases.push_back(make_flow_route_case());
	cases.push_back(make_flow_not_found_case());
#if CONFLUX_HAS_COMPRESS
	cases.push_back(make_codec_payload_case_owned("gzip", 512));
	cases.push_back(make_codec_payload_case_owned("gzip", 4096));
	cases.push_back(make_codec_payload_case_owned("gzip", 16384));
	cases.push_back(make_codec_payload_case_owned("gzip", 65536));
#endif
#if CONFLUX_HAS_ZSTD
	cases.push_back(make_codec_payload_case_owned("zstd", 512));
	cases.push_back(make_codec_payload_case_owned("zstd", 4096));
	cases.push_back(make_codec_payload_case_owned("zstd", 16384));
	cases.push_back(make_codec_payload_case_owned("zstd", 65536));
#endif
#if CONFLUX_HAS_BROTLI
	cases.push_back(make_codec_payload_case_owned("br", 512));
	cases.push_back(make_codec_payload_case_owned("br", 4096));
	cases.push_back(make_codec_payload_case_owned("br", 16384));
	cases.push_back(make_codec_payload_case_owned("br", 65536));
#endif
#if CONFLUX_HAS_COMPRESS
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::zlib, 512));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::zlib, 4096));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::zlib, 16384));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::zlib, 65536));
#endif
#if CONFLUX_HAS_LIBDEFLATE
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::libdeflate, 512));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::libdeflate, 4096));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::libdeflate, 16384));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::libdeflate, 65536));
#endif
#if CONFLUX_HAS_ZLIB_NG
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::zlib_ng, 512));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::zlib_ng, 4096));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::zlib_ng, 16384));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::zlib_ng, 65536));
#endif
#if CONFLUX_HAS_ISAL
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::isa_l, 512));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::isa_l, 4096));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::isa_l, 16384));
	cases.push_back(make_gzip_backend_payload_case(GzipBackend::isa_l, 65536));
#endif
	return cases;
}

} // namespace benchmark_detail

int main(
	int argc,
	char **argv) {
	try {
		auto const cfg = benchmark_detail::parse_args({argv, static_cast<size_t>(argc)});
		auto cases = benchmark_detail::build_cases();

		vector<benchmark_detail::Case const *> selected;
		selected.reserve(cases.size());
		for (auto const &bench: cases) {
			if (benchmark_detail::matches_filter(bench, cfg.filter)) {
				selected.push_back(&bench);
			}
		}

		if (cfg.list_only) {
			benchmark_detail::print_list(cases);
			return 0;
		}
		if (selected.empty()) {
			throw runtime_error{"no benchmark cases matched the current filter"};
		}

		benchmark_detail::print_header(cfg.format);
		for (auto const *bench: selected) {
			auto const iterations = cfg.iterations_override.value_or(bench->default_iterations);
			auto const stats = benchmark_detail::measure_case(*bench, iterations);
			benchmark_detail::print_stats(stats, cfg.format);
		}
		println("sink={}", benchmark_detail::sink.load(memory_order_relaxed));
		return 0;
	} catch (exception const &ex) {
		println(cerr, "conflux_benchmarks: {}", ex.what());
		benchmark_detail::print_usage();
		return 1;
	}
}
