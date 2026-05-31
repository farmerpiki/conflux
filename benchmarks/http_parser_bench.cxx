import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http1_parser;

import bench_common;

using namespace std::string_view_literals;
using conflux::http::ParserLimits;

namespace {

struct ParserCase {
	std::string_view name;
	std::string raw;
	conflux::http1::ParseStatus expected;
	ParserLimits limits{};
};

std::atomic<std::size_t> g_sink{};

ParserCase make_case(
	std::string_view name) {
	ParserLimits limits{};
	limits.max_request_line_size = 8192;
	limits.max_header_line_size = 8192;
	limits.max_header_block_size = 65536;
	limits.max_headers = 128;

	if (name == "valid_small"sv) {
		return {
			.name = name,
			.raw = "GET /api/users/42?active=true HTTP/1.1\r\nHost: localhost\r\nAccept: application/json\r\n\r\n",
			.expected = conflux::http1::ParseStatus::Ok,
			.limits = limits};
	}
	if (name == "headers_64"sv) {
		std::string raw = "GET /api/users/42 HTTP/1.1\r\nHost: localhost\r\n";
		for (int i = 0; i < 64; ++i) {
			raw += std::format("X-Bench-{}: value{}\r\n", i, i);
		}
		raw += "\r\n";
		return {.name = name, .raw = std::move(raw), .expected = conflux::http1::ParseStatus::Ok, .limits = limits};
	}
	if (name == "header_near_limit"sv) {
		std::string raw = "GET /large-header HTTP/1.1\r\nHost: localhost\r\nX-Large: ";
		raw.append(4096, 'a');
		raw += "\r\n\r\n";
		return {.name = name, .raw = std::move(raw), .expected = conflux::http1::ParseStatus::Ok, .limits = limits};
	}
	if (name == "malformed_bad_header"sv) {
		return {
			.name = name,
			.raw = "GET /bad HTTP/1.1\r\nHost: localhost\r\nBad Header: value\r\n\r\n",
			.expected = conflux::http1::ParseStatus::BadRequest,
			.limits = limits};
	}
	if (name == "incomplete_headers"sv) {
		std::string raw = "GET /slow HTTP/1.1\r\nHost: localhost\r\nX-Still-Growing: ";
		raw.append(2048, 's');
		return {
			.name = name,
			.raw = std::move(raw),
			.expected = conflux::http1::ParseStatus::Incomplete,
			.limits = limits};
	}
	throw std::invalid_argument{std::format("unknown parser case: {}", name)};
}

BenchStats bench_parser_case(
	ParserCase const &c,
	BenchSamplePlan const &plan) {
	conflux::http1::ParsedRequest parsed;
	auto stats = bench_measure_batched(
		[&] {
			auto const st = conflux::http1::parse_request(c.raw, c.limits, parsed);
			if (st != c.expected) {
				throw std::runtime_error{"unexpected parser status"};
			}
			g_sink.fetch_add(parsed.headers.size() + parsed.header_end_offset, std::memory_order_relaxed);
		},
		plan);
	stats.config = c.name;
	stats.variant = c.name;
	stats.throughput = stats.ns_per_iter > 0.0 ? 1e9 / stats.ns_per_iter : 0.0;
	return stats;
}

std::string_view parse_case_name(
	std::span<char *> args) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const arg{args[i]};
		if (arg == "--case"sv && i + 1 < args.size()) {
			return args[i + 1];
		}
	}
	return "valid_small"sv;
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"http_parser","parser":"standard","configs":[{"name":"valid_small","extra":{"kind":"user-space","case":"valid_small"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","valid_small","--config-name","valid_small","--iterations","0","--warmup","0"]},{"name":"headers_64","extra":{"kind":"adversarial-parser","case":"headers_64"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","headers_64","--config-name","headers_64","--iterations","0","--warmup","0"]},{"name":"header_near_limit","extra":{"kind":"adversarial-parser","case":"header_near_limit"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","header_near_limit","--config-name","header_near_limit","--iterations","0","--warmup","0"]},{"name":"malformed_bad_header","extra":{"kind":"adversarial-parser","case":"malformed_bad_header"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","malformed_bad_header","--config-name","malformed_bad_header","--iterations","0","--warmup","0"]},{"name":"incomplete_headers","extra":{"kind":"adversarial-parser","case":"incomplete_headers"},"target_ms":500,"max_iterations":10000000,"calibration_iterations":16,"args":["--case","incomplete_headers","--config-name","incomplete_headers","--iterations","0","--warmup","0"]}]})");

	auto const args = std::span{argv, static_cast<std::size_t>(argc)};
	auto cfg = bench_parse_args(args);
	auto c = make_case(parse_case_name(args));
	auto const plan = bench_sample_plan(cfg, 200000, 40000);
	auto stats = bench_parser_case(c, plan);
	bench_print(stats, cfg.json_out, true);
	std::println(std::cerr, "sink={}", g_sink.load(std::memory_order_relaxed));
}
