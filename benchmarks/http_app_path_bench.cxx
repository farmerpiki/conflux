#include <cstdlib>
#include <new>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http.types;
import conflux.net.http.server_types;
import conflux.net.http1_parser;
import conflux.net.http.parse_helpers;
import conflux.net.http.response;
import conflux.net.http_server_helpers;
import conflux.net.router;
import conflux.net.app;
import conflux.json;

import bench_common;

using namespace std::string_view_literals;
using namespace conflux::json;

namespace bench_alloc_detail {

thread_local bool count_enabled = false;
thread_local std::size_t allocation_count = 0;
thread_local std::size_t allocation_bytes = 0;

struct Snapshot {
	std::size_t count{};
	std::size_t bytes{};
};

void reset() noexcept {
	allocation_count = 0;
	allocation_bytes = 0;
}

[[nodiscard]] Snapshot snapshot() noexcept {
	return {.count = allocation_count, .bytes = allocation_bytes};
}

void record(
	std::size_t size) noexcept {
	if (count_enabled) {
		++allocation_count;
		allocation_bytes += size;
	}
}

class Scope {
	bool previous_;

public:
	Scope() noexcept
		: previous_(std::exchange(count_enabled, true)) {}
	~Scope() { count_enabled = previous_; }
	Scope(Scope const &) = delete;
	Scope &operator =(Scope const &) = delete;
};

} // namespace bench_alloc_detail

void *operator new(
	std::size_t size) {
	bench_alloc_detail::record(size);
	if (void *ptr = std::malloc(size == 0 ? 1 : size)) {
		return ptr;
	}
	throw std::bad_alloc{};
}

void *operator new[](
	std::size_t size) {
	bench_alloc_detail::record(size);
	if (void *ptr = std::malloc(size == 0 ? 1 : size)) {
		return ptr;
	}
	throw std::bad_alloc{};
}

void *operator new(
	std::size_t size,
	std::align_val_t alignment) {
	bench_alloc_detail::record(size);
	void *ptr = nullptr;
	if (posix_memalign(&ptr, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) == 0) {
		return ptr;
	}
	throw std::bad_alloc{};
}

void *operator new[](
	std::size_t size,
	std::align_val_t alignment) {
	return ::operator new(size, alignment);
}

void operator delete(
	void *ptr) noexcept {
	std::free(ptr);
}
void operator delete[](
	void *ptr) noexcept {
	std::free(ptr);
}
void operator delete(
	void *ptr,
	std::size_t) noexcept {
	std::free(ptr);
}
void operator delete[](
	void *ptr,
	std::size_t) noexcept {
	std::free(ptr);
}
void operator delete(
	void *ptr,
	std::align_val_t) noexcept {
	std::free(ptr);
}
void operator delete[](
	void *ptr,
	std::align_val_t) noexcept {
	std::free(ptr);
}
void operator delete(
	void *ptr,
	std::size_t,
	std::align_val_t) noexcept {
	std::free(ptr);
}
void operator delete[](
	void *ptr,
	std::size_t,
	std::align_val_t) noexcept {
	std::free(ptr);
}

namespace {

using conflux::http::ParserLimits;

enum class PathPhase : std::uint8_t {
	full,
	parse,
	route,
};

struct PathState {
	std::string name;
	std::string description;
	std::string raw;
	ParserLimits limits{};
	Router router;
	std::unique_ptr<conflux::http::App> app;
	bool route_response = true;
	bool serialize_response = true;
};

struct PathCase {
	std::string_view name;
	std::string_view description;
	std::size_t default_iterations;
	std::shared_ptr<PathState> state;
};

struct PathBenchStats {
	BenchStats timing{};
	double best_ns_per_iter{};
	double p10_ns_per_iter{};
	double p50_ns_per_iter{};
	double p99_ns_per_iter{};
	bool has_allocations = false;
	std::size_t allocations{};
	std::size_t allocation_bytes{};
};

struct BenchAppJsonBody {
	std::int64_t id{};
	std::int64_t count{};
	std::string name{};
	std::vector<std::int64_t> values{};
};

struct BenchAppFields {
	std::string q{};
	std::uint32_t page{};
	std::optional<std::uint64_t> limit{};
	bool active{};
	double score{};
};

} // namespace

template<>
struct conflux::json::JsonMembers<BenchAppJsonBody> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &BenchAppJsonBody::id),
			json_member("count", &BenchAppJsonBody::count),
			json_member("name", &BenchAppJsonBody::name),
			json_member("values", &BenchAppJsonBody::values),
		};
	}
	static constexpr std::string_view type_name() { return "BenchAppJsonBody"; }
};

template<>
struct conflux::json::JsonMembers<BenchAppFields> {
	static constexpr auto members() {
		return std::tuple{
			json_member("q", &BenchAppFields::q),
			json_member("page", &BenchAppFields::page),
			json_member("limit", &BenchAppFields::limit),
			json_member("active", &BenchAppFields::active),
			json_member("score", &BenchAppFields::score),
		};
	}
	static constexpr std::string_view type_name() { return "BenchAppFields"; }
};

namespace {

std::atomic<std::size_t> g_sink{};

[[nodiscard]] ParserLimits default_limits() {
	ParserLimits limits{};
	limits.max_request_line_size = 8192;
	limits.max_header_line_size = 8192;
	limits.max_header_block_size = 65536;
	limits.max_headers = 128;
	return limits;
}

[[nodiscard]] std::string make_get_request(
	std::string_view target,
	std::string_view extra_headers = {}) {
	std::string raw;
	raw.reserve(96 + target.size() + extra_headers.size());
	raw += "GET ";
	raw += target;
	raw += " HTTP/1.1\r\nHost: localhost\r\n";
	raw += extra_headers;
	raw += "\r\n";
	return raw;
}

[[nodiscard]] std::string make_post_request(
	std::string_view target,
	std::string_view body,
	std::string_view content_type = "text/plain"sv,
	std::string_view extra_headers = {}) {
	std::string raw;
	raw.reserve(160 + target.size() + body.size() + content_type.size() + extra_headers.size());
	raw += "POST ";
	raw += target;
	raw += " HTTP/1.1\r\nHost: localhost\r\nContent-Type: ";
	raw += content_type;
	raw += "\r\nContent-Length: ";
	raw += std::to_string(body.size());
	raw += "\r\n";
	raw += extra_headers;
	raw += "\r\n";
	raw += body;
	return raw;
}

[[nodiscard]] std::string make_multipart_body(
	std::string_view boundary,
	std::size_t text_parts,
	std::size_t file_parts,
	std::size_t file_bytes) {
	std::string raw;
	raw.reserve((text_parts + file_parts + 1) * 160 + file_parts * file_bytes);
	for (std::size_t i = 0; i < text_parts; ++i) {
		raw += "--";
		raw += boundary;
		raw += "\r\nContent-Disposition: form-data; name=\"field";
		raw += std::to_string(i);
		raw += "\"\r\n\r\nvalue-";
		raw += std::to_string(i);
		raw += "\r\n";
	}
	for (std::size_t i = 0; i < file_parts; ++i) {
		raw += "--";
		raw += boundary;
		raw += "\r\nContent-Disposition: form-data; name=\"upload";
		raw += std::to_string(i);
		raw += "\"; filename=\"file";
		raw += std::to_string(i);
		raw += ".txt\"\r\nContent-Type: text/plain\r\n\r\n";
		raw.append(file_bytes, static_cast<char>('a' + (i % 26)));
		raw += "\r\n";
	}
	raw += "--";
	raw += boundary;
	raw += "--\r\n";
	return raw;
}

void add_header_middlewares(
	Router &router,
	std::size_t count) {
	for (std::size_t i = 0; i < count; ++i) {
		router.use([i](RequestView const &req, Router::Handler const &next) {
			auto resp = next(req);
			resp.headers.set(std::format("X-Bench-Mw-{}", i), "1");
			return resp;
		});
	}
}

void append_standard_routes(
	PathState &state,
	std::string small_json,
	std::string medium_json) {
	state.router.get("/api/ping", [](RequestView const &) { return conflux::http::Response::text("pong"); });
	state.router.get("/hello/{name}", [](RequestView const &req) {
		return conflux::http::Response::text(std::format("hello {}", req.params["name"]));
	});
	state.router.get("/api/json-small", [body = std::move(small_json)](RequestView const &) {
		return conflux::http::Response::json(body);
	});
	state.router.get("/api/json-medium", [body = std::move(medium_json)](RequestView const &) {
		return conflux::http::Response::json(body);
	});
	state.router.post("/api/echo-size", [](RequestView const &req) {
		return conflux::http::Response::text(std::to_string(req.body.size()));
	});
}

[[nodiscard]] std::string make_medium_json() {
	std::string body = R"({"items":[)";
	for (int i = 0; i < 32; ++i) {
		if (i != 0) {
			body += ',';
		}
		body += std::format(R"({{"id":{},"name":"item-{}","ok":true}})", i, i);
	}
	body += R"(],"count":32})";
	return body;
}

[[nodiscard]] PathCase make_case(
	std::string_view name) {
	auto state = std::make_shared<PathState>();
	state->name = std::string{name};
	state->limits = default_limits();
	auto const small_json = std::string{R"({"ok":true,"value":42})"};
	auto medium_json = make_medium_json();

	if (name == "get_ping"sv) {
		state->description = "parse GET /api/ping, exact-route dispatch, text response, serialize";
		state->raw = make_get_request("/api/ping");
		append_standard_routes(*state, small_json, std::move(medium_json));
		return {state->name, state->description, 400000, state};
	}
	if (name == "get_param"sv) {
		state->description = "parse GET /hello/{name}, parameter route, text response, serialize";
		state->raw = make_get_request("/hello/alice");
		append_standard_routes(*state, small_json, std::move(medium_json));
		return {state->name, state->description, 300000, state};
	}
	if (name == "not_found"sv) {
		state->description = "parse missing GET route, 404 response build, serialize";
		state->raw = make_get_request("/api/not-found");
		append_standard_routes(*state, small_json, std::move(medium_json));
		return {state->name, state->description, 300000, state};
	}
	if (name == "middleware_x1"sv || name == "middleware_x4"sv || name == "middleware_x16"sv) {
		std::size_t const count = name == "middleware_x1"sv ? 1 : (name == "middleware_x4"sv ? 4 : 16);
		state->description =
			std::format("parse exact GET through {} header middlewares, response build, serialize", count);
		state->raw = make_get_request("/api/ping");
		add_header_middlewares(state->router, count);
		append_standard_routes(*state, small_json, std::move(medium_json));
		return {state->name, state->description, count >= 16 ? std::size_t{100000} : std::size_t{200000}, state};
	}
	if (name == "json_small"sv) {
		state->description = "parse GET JSON-small route, response build, serialize";
		state->raw = make_get_request("/api/json-small");
		append_standard_routes(*state, small_json, std::move(medium_json));
		return {state->name, state->description, 250000, state};
	}
	if (name == "json_medium"sv) {
		state->description = "parse GET JSON-medium route, response build, serialize";
		state->raw = make_get_request("/api/json-medium");
		append_standard_routes(*state, small_json, std::move(medium_json));
		return {state->name, state->description, 100000, state};
	}
	if (name == "post_body_parse_only"sv) {
		std::string body;
		body.reserve(4096);
		for (int i = 0; i < 256; ++i) {
			if (i != 0) {
				body += '&';
			}
			body += std::format("k{}={}", i, i);
		}
		state->description = "parse POST headers/body and urlencoded form only, no route/serialize";
		state->raw = make_post_request("/api/echo-size", body, "application/x-www-form-urlencoded");
		state->route_response = false;
		state->serialize_response = false;
		return {state->name, state->description, 120000, state};
	}
	if (name == "post_echo"sv) {
		std::string body(4096, 'x');
		state->description = "parse POST 4 KiB body, route echo-size response, serialize";
		state->raw = make_post_request("/api/echo-size", body);
		append_standard_routes(*state, small_json, std::move(medium_json));
		return {state->name, state->description, 120000, state};
	}
	if (name == "app_json_body"sv) {
		auto body = std::string{R"({"id":7,"count":42,"name":"bench","values":[1,2,3,4,5,6,7,8]})"};
		state->description = "parse POST JSON body through App Json<T> extractor, route response, serialize";
		state->raw = make_post_request("/api/json-body", body, "application/json");
		state->app = std::make_unique<conflux::http::App>();
		state->app->post("/api/json-body", [](conflux::http::Json<BenchAppJsonBody> const &json) {
			auto const &body = *json;
			return conflux::http::Response::text(
				std::to_string(body.id + body.count + static_cast<std::int64_t>(body.values.size())));
		});
		return {state->name, state->description, 120000, state};
	}
	if (name == "app_query_params"sv) {
		state->description = "parse GET query fields through App QueryParams<T> extractor, route response, serialize";
		state->raw = make_get_request("/api/query-fields?q=bench&page=3&limit=42&active=true&score=12.5");
		state->app = std::make_unique<conflux::http::App>();
		state->app->get("/api/query-fields", [](conflux::http::QueryParams<BenchAppFields> const &query) {
			auto const &fields = *query;
			auto const limit = fields.limit.value_or(0);
			return conflux::http::Response::text(
				std::format("{}:{}:{}:{}", fields.q, fields.page, limit, fields.score));
		});
		return {state->name, state->description, 160000, state};
	}
	if (name == "app_form_params"sv) {
		auto body = std::string{"q=bench&page=3&limit=42&active=true&score=12.5"};
		state->description = "parse POST form fields through App FormParams<T> extractor, route response, serialize";
		state->raw = make_post_request("/api/form-fields", body, "application/x-www-form-urlencoded");
		state->app = std::make_unique<conflux::http::App>();
		state->app->post("/api/form-fields", [](conflux::http::FormParams<BenchAppFields> const &form) {
			auto const &fields = *form;
			auto const limit = fields.limit.value_or(0);
			return conflux::http::Response::text(
				std::format("{}:{}:{}:{}", fields.q, fields.page, limit, fields.score));
		});
		return {state->name, state->description, 120000, state};
	}
	if (name == "multipart_mixed"sv) {
		static constexpr std::string_view kBoundary = "benchBoundary42";
		auto body = make_multipart_body(kBoundary, 8, 2, 2048);
		state->description = "parse multipart/form-data with text fields and files, route counts, serialize";
		state->raw = make_post_request("/api/multipart-counts", body, "multipart/form-data; boundary=benchBoundary42");
		state->router.post("/api/multipart-counts", [](RequestView const &req) {
			std::size_t file_bytes = 0;
			for (auto const &file: req.files) {
				file_bytes += file.data.size();
			}
			return conflux::http::Response::text(
				std::format("{}:{}:{}", req.form.size(), req.files.size(), file_bytes));
		});
		return {state->name, state->description, 60000, state};
	}
	throw std::invalid_argument{std::format("unknown HTTP app-path case: {}", name)};
}

[[nodiscard]] std::vector<std::string_view> all_case_names() {
	return {
		"get_ping"sv,
		"get_param"sv,
		"not_found"sv,
		"middleware_x1"sv,
		"middleware_x4"sv,
		"middleware_x16"sv,
		"json_small"sv,
		"json_medium"sv,
		"post_body_parse_only"sv,
		"post_echo"sv,
		"app_json_body"sv,
		"app_query_params"sv,
		"app_form_params"sv,
		"multipart_mixed"sv,
	};
}

[[nodiscard]] PathPhase parse_phase(
	std::span<char *> args) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const arg{args[i]};
		if (arg == "--phase"sv && i + 1 < args.size()) {
			std::string_view const value{args[i + 1]};
			if (value == "full"sv || value == "serialize"sv) {
				return PathPhase::full;
			}
			if (value == "parse"sv) {
				return PathPhase::parse;
			}
			if (value == "route"sv || value == "dispatch"sv) {
				return PathPhase::route;
			}
			throw std::invalid_argument{"--phase must be full, parse, or route"};
		}
	}
	return PathPhase::full;
}

void apply_phase(
	PathState &state,
	PathPhase phase) {
	if (phase == PathPhase::parse) {
		state.route_response = false;
		state.serialize_response = false;
		return;
	}
	if (phase == PathPhase::route) {
		state.route_response = true;
		state.serialize_response = false;
	}
}

[[nodiscard]] std::string_view parse_case_name(
	std::span<char *> args) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const arg{args[i]};
		if (arg == "--case"sv && i + 1 < args.size()) {
			return args[i + 1];
		}
	}
	return "get_ping"sv;
}

[[nodiscard]] std::vector<PathCase> selected_cases(
	std::span<char *> args,
	PathPhase phase) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const arg{args[i]};
		if (arg == "--all-cases"sv) {
			std::vector<PathCase> out;
			for (auto name: all_case_names()) {
				auto c = make_case(name);
				apply_phase(*c.state, phase);
				out.push_back(std::move(c));
			}
			return out;
		}
	}
	auto c = make_case(parse_case_name(args));
	apply_phase(*c.state, phase);
	return {std::move(c)};
}

[[nodiscard]] RequestView make_request_view(
	std::string_view raw,
	ParserLimits const &limits,
	conflux::http1::ParsedRequest &parsed,
	HttpFieldsView &headers,
	HttpFieldsView &query,
	HttpFieldsView &form,
	HttpFieldsView &cookies,
	std::vector<conflux::http::UploadedFile> &files,
	std::string_view &path,
	std::string_view &body) {
	auto const status = conflux::http1::parse_request(raw, limits, parsed);
	if (status != conflux::http1::ParseStatus::Ok) {
		throw std::runtime_error{"HTTP app-path parser did not return Ok"};
	}

	auto const target = conflux::http::split_path_query(parsed.target);
	path = conflux::http::origin_form_path_from_target(target.path);
	body = raw.substr(parsed.header_end_offset + 4);

	headers.clear();
	headers.reserve(parsed.headers.size());
	for (auto const &[name, field_value]: parsed.headers) {
		headers.emplace_back(name, field_value);
	}

	query.clear();
	if (!target.query.empty()) {
		parse_urlencoded(target.query, query);
	}

	cookies.clear();
	if (auto cookie = headers.get("cookie"); cookie.has_value()) {
		parse_cookies(*cookie, cookies);
	}

	form.clear();
	if (conflux::http::ascii_iequals(headers["content-type"], "application/x-www-form-urlencoded")) {
		parse_urlencoded(body, form);
	}

	files.clear();
	if (content_type_is_multipart_form_data(headers["content-type"])) {
		auto const boundary = extract_param(headers["content-type"], "boundary");
		if (!boundary.empty()) {
			parse_multipart(body, boundary, form, files);
		}
	}

	return RequestView{
		parsed.method,
		path,
		parsed.version,
		"127.0.0.1"sv,
		false,
		HttpFieldsView{},
		headers,
		query,
		form,
		cookies,
		files,
		body};
}

[[nodiscard]] std::size_t run_one(
	PathState &state) {
	conflux::http1::ParsedRequest parsed;
	HttpFieldsView headers{true};
	HttpFieldsView query;
	HttpFieldsView form;
	HttpFieldsView cookies;
	std::vector<conflux::http::UploadedFile> files;
	std::string_view path;
	std::string_view body;
	auto req = make_request_view(state.raw, state.limits, parsed, headers, query, form, cookies, files, path, body);

	if (!state.route_response) {
		return req.method.size() + req.path.size() + req.body.size() + req.form.size() + req.files.size();
	}

	auto resp = state.app ? conflux::http::router(*state.app).dispatch(req) : state.router.dispatch(req);
	if (!state.serialize_response) {
		return resp.text_body().size() + static_cast<std::size_t>(resp.status);
	}
	return format_response(resp, {}, false).size();
}

[[nodiscard]] PathBenchStats bench_case(
	PathCase const &c,
	BenchSamplePlan const &plan,
	bool count_allocations) {
	for (std::size_t i = 0; i < plan.warmup_samples; ++i) {
		for (std::size_t j = 0; j < plan.batch; ++j) {
			g_sink.fetch_add(run_one(*c.state), std::memory_order_relaxed);
		}
	}

	std::vector<std::uint64_t> samples;
	samples.reserve(plan.samples);
	std::uint64_t total_ns = 0;
	for (std::size_t i = 0; i < plan.samples; ++i) {
		auto const t0 = bench_now_ns();
		for (std::size_t j = 0; j < plan.batch; ++j) {
			g_sink.fetch_add(run_one(*c.state), std::memory_order_relaxed);
		}
		auto const elapsed = bench_now_ns() - t0;
		total_ns += elapsed;
		samples.push_back(elapsed);
	}
	std::ranges::sort(samples);
	auto sample_ns = [&](std::size_t idx) {
		return static_cast<double>(samples[std::min(idx, samples.size() - 1U)]) / static_cast<double>(plan.batch);
	};
	double const best_ns = sample_ns(0);
	double const p10_ns = sample_ns(plan.samples / 10U);
	double const p50_ns = sample_ns(plan.samples / 2U);
	double const p99_ns = sample_ns((plan.samples * 99U) / 100U);
	PathBenchStats stats{
		.timing =
			{.config = c.name,
					 .variant = c.name,
					 .iterations = plan.iterations,
					 .total_ns = total_ns,
					 .ns_per_iter = p50_ns,
					 .throughput = 1e9 / p50_ns},
		.best_ns_per_iter = best_ns,
		.p10_ns_per_iter = p10_ns,
		.p50_ns_per_iter = p50_ns,
		.p99_ns_per_iter = p99_ns,
	};
	bench_apply_sample_plan(stats.timing, plan);

	if (count_allocations) {
		bench_alloc_detail::reset();
		{
			bench_alloc_detail::Scope const scope;
			for (std::size_t i = 0; i < plan.iterations; ++i) {
				g_sink.fetch_add(run_one(*c.state), std::memory_order_relaxed);
			}
		}
		auto const allocations = bench_alloc_detail::snapshot();
		stats.has_allocations = true;
		stats.allocations = allocations.count;
		stats.allocation_bytes = allocations.bytes;
	}
	return stats;
}

void print_path_stats(
	PathBenchStats const &s,
	bool json_out,
	bool first) {
	auto const print_json_common = [&] {
		std::print(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"best_ns_per_iter\":{:.2f},\"p10_ns_per_iter\":{:.2f},\"p50_ns_per_iter\":{:.2f},"
			"\"p99_ns_per_iter\":{:.2f},\"sample_count\":{},\"batch\":{},\"timer_sample_ns\":{},"
			"\"timer_overhead_pct\":{:.4f}",
			s.timing.config,
			s.timing.variant,
			s.timing.iterations,
			s.timing.total_ns,
			s.timing.ns_per_iter,
			s.best_ns_per_iter,
			s.p10_ns_per_iter,
			s.p50_ns_per_iter,
			s.p99_ns_per_iter,
			s.timing.sample_count,
			s.timing.batch,
			s.timing.timer_sample_ns,
			s.timing.timer_overhead_pct);
	};
	if (!s.has_allocations) {
		if (json_out) {
			print_json_common();
			std::println("}}");
			(void)first;
			return;
		}
		std::println(
			"[{}] {:<24} {:>10} iters  {:>9.2f} ns/iter  best {:>9.2f}  p10 {:>9.2f}  p99 {:>9.2f}  "
			"{:>12.0f} ops/s  [{}×{} samples, timer≈{:.2f}%]",
			s.timing.config,
			s.timing.variant,
			s.timing.iterations,
			s.timing.ns_per_iter,
			s.best_ns_per_iter,
			s.p10_ns_per_iter,
			s.p99_ns_per_iter,
			s.timing.throughput,
			s.timing.sample_count,
			s.timing.batch,
			s.timing.timer_overhead_pct);
		return;
	}
	if (json_out) {
		print_json_common();
		std::println(
			",\"allocations\":{},\"allocation_bytes\":{},\"allocs_per_iter\":{:.4f},\"allocation_bytes_per_iter\":{:."
			"2f}"
			"}}",
			s.allocations,
			s.allocation_bytes,
			static_cast<double>(s.allocations) / static_cast<double>(s.timing.iterations),
			static_cast<double>(s.allocation_bytes) / static_cast<double>(s.timing.iterations));
		(void)first;
		return;
	}
	std::println(
		"[{}] {:<24} {:>10} iters  {:>9.2f} ns/iter  best {:>9.2f}  p10 {:>9.2f}  p99 {:>9.2f}  "
		"{:>12.0f} ops/s  [{}×{} samples, timer≈{:.2f}%]",
		s.timing.config,
		s.timing.variant,
		s.timing.iterations,
		s.timing.ns_per_iter,
		s.best_ns_per_iter,
		s.p10_ns_per_iter,
		s.p99_ns_per_iter,
		s.timing.throughput,
		s.timing.sample_count,
		s.timing.batch,
		s.timing.timer_overhead_pct);
	std::println(
		"    allocations: {:.4f}/iter  bytes: {:.2f}/iter",
		static_cast<double>(s.allocations) / static_cast<double>(s.timing.iterations),
		static_cast<double>(s.allocation_bytes) / static_cast<double>(s.timing.iterations));
}

void print_list() {
	for (auto name: all_case_names()) {
		auto c = make_case(name);
		std::println("{:<24} {}", c.name, c.description);
	}
}

void print_usage() {
	std::println(
		"Usage: conflux_http_app_path_bench [--case NAME|--all-cases] [--phase full|parse|route] [--count-allocs] "
		"[--iterations N] [--warmup N] [--samples N] [--batch N] [--json] [--list]");
}

[[nodiscard]] bool has_flag(
	std::span<char *> args,
	std::string_view wanted) {
	for (auto const *arg: args.subspan(1)) {
		if (std::string_view{arg} == wanted) {
			return true;
		}
	}
	return false;
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"http_app_path","parser":"standard","configs":[{"name":"get_ping","extra":{"kind":"micro/user-space","case":"GET /api/ping"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","get_ping","--config-name","get_ping","--iterations","0","--warmup","0"]},{"name":"get_param","extra":{"kind":"micro/user-space","case":"GET /hello/{name}"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","get_param","--config-name","get_param","--iterations","0","--warmup","0"]},{"name":"not_found","extra":{"kind":"micro/user-space","case":"404"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","not_found","--config-name","not_found","--iterations","0","--warmup","0"]},{"name":"middleware_x1","extra":{"kind":"micro/user-space","middleware_count":1},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","middleware_x1","--config-name","middleware_x1","--iterations","0","--warmup","0"]},{"name":"middleware_x4","extra":{"kind":"micro/user-space","middleware_count":4},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","middleware_x4","--config-name","middleware_x4","--iterations","0","--warmup","0"]},{"name":"middleware_x16","extra":{"kind":"micro/user-space","middleware_count":16},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","middleware_x16","--config-name","middleware_x16","--iterations","0","--warmup","0"]},{"name":"json_small","extra":{"kind":"micro/user-space","case":"JSON response small"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","json_small","--config-name","json_small","--iterations","0","--warmup","0"]},{"name":"json_medium","extra":{"kind":"micro/user-space","case":"JSON response medium"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","json_medium","--config-name","json_medium","--iterations","0","--warmup","0"]},{"name":"post_body_parse_only","extra":{"kind":"micro/user-space","case":"POST body parse only"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","post_body_parse_only","--config-name","post_body_parse_only","--iterations","0","--warmup","0"]},{"name":"post_echo","extra":{"kind":"micro/user-space","case":"POST echo 4KiB"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","post_echo","--config-name","post_echo","--iterations","0","--warmup","0"]},{"name":"app_json_body","extra":{"kind":"micro/user-space","case":"App Json<T> request body"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","app_json_body","--config-name","app_json_body","--iterations","0","--warmup","0"]},{"name":"app_query_params","extra":{"kind":"micro/user-space","case":"App QueryParams<T> aggregate fields"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","app_query_params","--config-name","app_query_params","--iterations","0","--warmup","0"]},{"name":"app_form_params","extra":{"kind":"micro/user-space","case":"App FormParams<T> aggregate fields"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","app_form_params","--config-name","app_form_params","--iterations","0","--warmup","0"]},{"name":"multipart_mixed","extra":{"kind":"micro/user-space","case":"multipart/form-data text and file parts"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","multipart_mixed","--config-name","multipart_mixed","--iterations","0","--warmup","0"]}]})");

	try {
		auto const args = std::span{argv, static_cast<std::size_t>(argc)};
		if (has_flag(args, "--help"sv) || has_flag(args, "-h"sv)) {
			print_usage();
			return 0;
		}
		if (has_flag(args, "--list"sv)) {
			print_list();
			return 0;
		}

		auto const cfg = bench_parse_args(args);
		auto const phase = parse_phase(args);
		auto const count_allocations = has_flag(args, "--count-allocs"sv) || has_flag(args, "--allocs"sv);
		auto cases = selected_cases(args, phase);
		for (auto const &c: cases) {
			auto const iterations = cfg.iterations == 0 ? c.default_iterations : cfg.iterations;
			auto const plan = bench_sample_plan(iterations, cfg.warmup, cfg.samples, cfg.batch);
			auto const stats = bench_case(c, plan, count_allocations);
			print_path_stats(stats, cfg.json_out, c.name == cases.front().name);
		}
		std::println(std::cerr, "sink={}", g_sink.load(std::memory_order_relaxed));
		return 0;
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_http_app_path_bench: {}", ex.what());
		print_usage();
		return 1;
	}
}
