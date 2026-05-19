import std;
import conflux.types;

import conflux.net.http;
namespace benchmark_detail {

inline std::atomic<std::size_t> sink{};
struct Config {
	bool list_only = false;
	std::string filter;
	std::optional<std::size_t> iterations_override;
	enum class Format : std::uint8_t {
		table,
		json,
	};
	Format format = Format::table;
};
struct Stats {
	std::string_view name;
	std::size_t iterations{};
	std::uint64_t total_ns{};
	double ns_per_iter{};
};
using BenchFn = std::function<std::size_t()>;
struct Case {
	std::string_view name;
	std::string_view description;
	std::size_t default_iterations;
	BenchFn run;
};
[[gnu::const]] std::size_t iterations_for_payload(
	std::size_t payload_size) {
	if (payload_size <= 512) {
		return 50000;
	}
	if (payload_size <= 4096) {
		return 20000;
	}
	if (payload_size <= 16384) {
		return 5000;
	}
	return 1000;
}
bool force_gzip_backend(
	GzipBackend backend) {
	set_compression_calibration(CompressionCalibration::disabled);
	return set_gzip_backend(backend);
}
void print_usage() {
	std::println("Usage: conflux_benchmarks [--list] [--filter SUBSTR] [--iterations N] [--format table|json]");
}
Config parse_args(
	std::span<char *> args) {
	Config cfg;
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view arg = args[i];
		if (arg == "--list") {
			cfg.list_only = true;
			continue;
		}
		if (arg == "--help" || arg == "-h") {
			print_usage();
			std::exit(0);
		}
		if (arg == "--filter") {
			if (i + 1 >= args.size()) {
				throw std::invalid_argument{"--filter requires a value"};
			}
			cfg.filter = args[++i];
			continue;
		}
		if (arg == "--iterations") {
			if (i + 1 >= args.size()) {
				throw std::invalid_argument{"--iterations requires a value"};
			}
			std::size_t iters = 0;
			auto const value = std::string_view{args[++i]};
			auto const [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), iters);
			if (ec != std::errc{} || ptr != value.data() + value.size() || iters == 0) {
				throw std::invalid_argument{"--iterations must be a positive integer"};
			}
			cfg.iterations_override = iters;
			continue;
		}
		if (arg == "--format") {
			if (i + 1 >= args.size()) {
				throw std::invalid_argument{"--format requires a value"};
			}
			auto const value = std::string_view{args[++i]};
			if (value == "table") {
				cfg.format = Config::Format::table;
			} else if (value == "json") {
				cfg.format = Config::Format::json;
			} else {
				throw std::invalid_argument{"--format must be table or json"};
			}
			continue;
		}
		if (arg == "--json") {
			cfg.format = Config::Format::json;
			continue;
		}
		throw std::invalid_argument{std::format("unknown argument: {}", arg)};
	}
	return cfg;
}
[[gnu::pure]] bool matches_filter(
	Case const &bench,
	std::string_view filter) {
	return filter.empty() || bench.name.contains(filter) || bench.description.contains(filter);
}
[[gnu::const]] std::size_t warmup_iterations(
	std::size_t iterations) {
	return std::clamp(iterations / 10, std::size_t{1}, std::size_t{1000});
}
Stats measure_case(
	Case const &bench,
	std::size_t iterations) {
	for (std::size_t i = 0; i < warmup_iterations(iterations); ++i) {
		sink.fetch_add(bench.run(), std::memory_order_relaxed);
	}

	auto const start = std::chrono::steady_clock::now();
	for (std::size_t i = 0; i < iterations; ++i) {
		sink.fetch_add(bench.run(), std::memory_order_relaxed);
	}
	auto const elapsed = std::chrono::steady_clock::now() - start;
	auto const total_ns =
		static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
	return Stats{
		.name = bench.name,
		.iterations = iterations,
		.total_ns = total_ns,
		.ns_per_iter = static_cast<double>(total_ns) / static_cast<double>(iterations)};
}
void print_list(
	std::vector<Case> const &cases) {
	for (auto const &bench: cases) {
		std::println("{:32} {}", bench.name, bench.description);
	}
}
void print_header(
	Config::Format format) {
	if (format == Config::Format::table) {
		std::println("{:32} {:>12} {:>14} {:>14}", "Benchmark", "Iterations", "Total (ms)", "ns/iter");
	}
}
void print_stats(
	Stats const &stats,
	Config::Format format) {
	if (format == Config::Format::table) {
		auto const total_ms = static_cast<double>(stats.total_ns) / 1'000'000.0;
		std::println("{:32} {:>12} {:>14.3f} {:>14.1f}", stats.name, stats.iterations, total_ms, stats.ns_per_iter);
	} else {
		std::println(
			"{{\"config\":\"\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f}}}",
			stats.name,
			stats.iterations,
			stats.total_ns,
			stats.ns_per_iter);
	}
}
Case make_httpfields_lookup_case() {
	auto headers = std::make_shared<HttpFields>(true);
	headers->emplace_back("Content-Type", "application/json");
	headers->emplace_back("Accept-Encoding", "br, zstd, gzip");
	headers->emplace_back("X-Request-Id", "abc-123");
	headers->emplace_back("X-Forwarded-For", "127.0.0.1");
	return Case{
		.name = "micro/httpfields_lookup",
		.description = "Case-insensitive hot-path header lookup",
		.default_iterations = 2000000,
		.run = [headers] {
			return headers->operator []("accept-encoding").size()
				 + headers->operator []("content-type").size()
				 + headers->operator []("missing").size();
		}};
}
Case make_typed_field_extract_case() {
	auto req = std::make_shared<HttpRequest>();
	req->headers.set("X-Limit", "128");
	req->query.emplace_back("page", "42");
	req->query.emplace_back("enabled", "true");
	req->cookies.emplace_back("sid", "abc-123");
	return Case{
		.name = "micro/typed_field_extract",
		.description = "Typed request field extraction from cached request fields",
		.default_iterations = 2000000,
		.run = [req] {
			auto page = req->query_as<std::uint32_t>("page");
			auto limit = req->header_as<std::uint32_t>("x-limit");
			auto enabled = req->query_as<bool>("enabled");
			auto sid = req->cookie_as<std::string_view>("sid");
			return static_cast<std::size_t>(page.value_or(0))
				 + static_cast<std::size_t>(limit.value_or(0))
				 + static_cast<std::size_t>(enabled.value_or(false))
				 + sid.value_or(std::string_view{}).size();
		}};
}
Case make_router_exact_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = std::make_shared<State>();
	state->router.get("/health", [](HttpRequestView const &) { return HttpResponse::text("ok"); });
	state->req.method = "GET";
	state->req.path = "/health";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "micro/router_dispatch_exact",
		.description = "Exact-path route dispatch without middleware",
		.default_iterations = 500000,
		.run = [state] { return state->router.dispatch(state->req).text_body().size(); }};
}
Case make_router_params_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = std::make_shared<State>();
	state->router.get("/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::text(std::format("{}:{}", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "micro/router_dispatch_params",
		.description = "Parameterized route matching with two captures",
		.default_iterations = 300000,
		.run = [state] { return state->router.dispatch(state->req).text_body().size(); }};
}
Case make_compress_case() {
	struct State {
		Router router;
		HttpRequest req;
		std::shared_ptr<std::string> payload{std::make_shared<std::string>()};
	};
	auto state = std::make_shared<State>();
	state->payload->assign(4096, 'x');
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) {
		return HttpResponse::text(*payload);
	});
	state->req.method = "GET";
	state->req.path = "/data";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "br, zstd, gzip";
	return Case{
		.name = "micro/compress_middleware",
		.description = "Compression negotiation and body rewrite on a text response",
		.default_iterations = 20000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}
Case make_compress_negotiation_miss_case() {
	struct State {
		Router router;
		HttpRequest req;
		std::shared_ptr<std::string> payload{std::make_shared<std::string>()};
	};
	auto state = std::make_shared<State>();
	state->payload->assign(4096, 'x');
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) {
		return HttpResponse::text(*payload);
	});
	state->req.method = "GET";
	state->req.path = "/data";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "identity";
	return Case{
		.name = "micro/compress_negotiation_miss",
		.description = "Compression middleware with no matching encoding",
		.default_iterations = 200000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}
Case make_compress_below_threshold_case() {
	struct State {
		Router router;
		HttpRequest req;
		std::shared_ptr<std::string> payload{std::make_shared<std::string>()};
	};
	auto state = std::make_shared<State>();
	state->payload->assign(128, 'x');
	state->router.use(compress_middleware({.min_body_size = 256}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) {
		return HttpResponse::text(*payload);
	});
	state->req.method = "GET";
	state->req.path = "/data";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "br, zstd, gzip";
	return Case{
		.name = "micro/compress_below_threshold",
		.description = "Compression middleware fast path for bodies below min size",
		.default_iterations = 300000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return resp.text_body().size() + resp.headers["Content-Encoding"].size();
		}};
}
Case make_codec_payload_case_owned(
	std::string codec_name,
	std::size_t payload_size) {
	struct State {
		std::string name;
		std::string description;
		Router router;
		HttpRequest req;
		std::shared_ptr<std::string> payload{std::make_shared<std::string>()};
	};
	auto state = std::make_shared<State>();
	state->name = std::format("codec/{}/{}B", codec_name, payload_size);
	state->description =
		std::format("Compression path pinned to {} for {} std::byte text payloads", codec_name, payload_size);
	state->payload->assign(payload_size, 'x');
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) {
		return HttpResponse::text(*payload);
	});
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
	std::size_t payload_size) {
	struct State {
		std::string name;
		std::string description;
		Router router;
		HttpRequest req;
		std::shared_ptr<std::string> payload{std::make_shared<std::string>()};
		GzipBackend backend;
		bool configured = false;
	};
	auto state = std::make_shared<State>();
	state->backend = backend;
	state->name = std::format("backend/{}/{}B", gzip_backend_name(backend), payload_size);
	state->description =
		std::format("Gzip backend {} on {} std::byte text payloads", gzip_backend_name(backend), payload_size);
	state->payload->assign(payload_size, 'x');
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/data", [payload = state->payload](HttpRequestView const &) {
		return HttpResponse::text(*payload);
	});
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
				return std::size_t{0};
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
	auto state = std::make_shared<State>();
	state->router.get("/api/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::json(
			std::format(R"({{"user":"{}","post":"{}","ok":true}})", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/api/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "flow/route_json_only",
		.description = "Route params plus JSON body construction without middleware",
		.default_iterations = 200000,
		.run = [state] { return state->router.dispatch(state->req).text_body().size(); }};
}
Case make_route_json_with_header_middleware_case() {
	struct State {
		Router router;
		HttpRequest req;
	};
	auto state = std::make_shared<State>();
	state->router.use([](HttpRequestView const &req, Router::Handler const &next) {
		auto resp = next(req);
		resp.headers["X-Bench"] = "flow";
		return resp;
	});
	state->router.get("/api/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::json(
			std::format(R"({{"user":"{}","post":"{}","ok":true}})", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/api/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "flow/route_json_with_header_mw",
		.description = "Route params and JSON construction with a lightweight middleware wrapper",
		.default_iterations = 150000,
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
	auto state = std::make_shared<State>();
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/api/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::json(
			std::format(R"({{"user":"{}","post":"{}","ok":true}})", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/api/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "identity";
	return Case{
		.name = "flow/route_json_with_compress_miss",
		.description = "Route params and JSON body through compression middleware with no matching codec",
		.default_iterations = 150000,
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
	auto state = std::make_shared<State>();
	state->router.use([](HttpRequestView const &req, Router::Handler const &next) {
		auto resp = next(req);
		resp.headers["X-Bench"] = "flow";
		return resp;
	});
	state->router.use(compress_middleware({.min_body_size = 0}));
	state->router.get("/api/users/{user}/posts/{post}", [](HttpRequestView const &req) {
		return HttpResponse::json(
			std::format(R"({{"user":"{}","post":"{}","ok":true}})", req.params["user"], req.params["post"]));
	});
	state->req.method = "GET";
	state->req.path = "/api/users/alice/posts/42";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	state->req.headers["Accept-Encoding"] = "br, zstd, gzip";
	return Case{
		.name = "flow/api_route_with_middleware",
		.description = "Full in-process request flow through middleware, params, and response shaping",
		.default_iterations = 100000,
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
	auto state = std::make_shared<State>();
	state->router.use([](HttpRequestView const &req, Router::Handler const &next) { return next(req); });
	state->router.get("/api/health", [](HttpRequestView const &) { return HttpResponse::text("ok"); });
	state->req.method = "GET";
	state->req.path = "/api/missing/resource";
	state->req.version = "HTTP/1.1";
	state->req.remote_addr = "127.0.0.1";
	return Case{
		.name = "flow/not_found_request",
		.description = "Miss path through the router stack",
		.default_iterations = 200000,
		.run = [state] {
			auto resp = state->router.dispatch(state->req);
			return static_cast<std::size_t>(resp.status) + resp.text_body().size();
		}};
}
std::vector<Case> build_cases() {
	std::vector<Case> cases;
	cases.push_back(make_httpfields_lookup_case());
	cases.push_back(make_typed_field_extract_case());
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
	if (argc >= 2 && std::string_view{argv[1]} == "--bench-info") {
		std::print(
			"{}\n",
			R"({"name":"router","parser":"standard","configs":[{"name":"default","extra":{},"args":[]}]})");
		return 0;
	}
	try {
		auto const cfg = benchmark_detail::parse_args({argv, static_cast<std::size_t>(argc)});
		auto cases = benchmark_detail::build_cases();

		std::vector<benchmark_detail::Case const *> selected;
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
			throw std::runtime_error{"no benchmark cases matched the current filter"};
		}

		benchmark_detail::print_header(cfg.format);
		for (auto const *bench: selected) {
			auto const iterations = cfg.iterations_override.value_or(bench->default_iterations);
			auto const stats = benchmark_detail::measure_case(*bench, iterations);
			benchmark_detail::print_stats(stats, cfg.format);
		}
		std::println(std::cerr, "sink={}", benchmark_detail::sink.load(std::memory_order_relaxed));
		return 0;
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_benchmarks: {}", ex.what());
		benchmark_detail::print_usage();
		return 1;
	}
}
