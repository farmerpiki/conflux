#include <atomic>
#include <cstdlib>
#include <new>
#include <sys/resource.h>

import std;
import conflux.types;
import conflux.net.config;
import conflux.net.http.types;
import conflux.net.http1_parser;
import conflux.net.http.parse_helpers;
import conflux.net.http.server_types;

import bench_common;

using namespace std::string_view_literals;
using conflux::http::HttpRejectReason;
using conflux::http::ParserLimits;

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_allocated_bytes{0};
std::atomic<std::size_t> g_sink{0};

void note_allocation(
	std::size_t size) noexcept {
	if (g_count_allocations.load(std::memory_order_relaxed)) {
		g_allocations.fetch_add(1, std::memory_order_relaxed);
		g_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
	}
}

void reset_allocation_counters() noexcept {
	g_allocations.store(0, std::memory_order_relaxed);
	g_allocated_bytes.store(0, std::memory_order_relaxed);
}

[[nodiscard]] void *allocate_counted(
	std::size_t size) {
	if (size == 0) {
		size = 1;
	}
	note_allocation(size);
	if (auto *p = std::malloc(size); p != nullptr) {
		return p;
	}
	throw std::bad_alloc{};
}

[[nodiscard]] void *allocate_counted_aligned(
	std::size_t size,
	std::size_t align) {
	if (size == 0) {
		size = 1;
	}
	if (align < alignof(void *)) {
		align = alignof(void *);
	}
	note_allocation(size);
	void *p{};
	if (posix_memalign(&p, align, size) == 0 && p != nullptr) {
		return p;
	}
	throw std::bad_alloc{};
}

} // namespace

void *operator new(
	std::size_t size) {
	return allocate_counted(size);
}
void *operator new[](
	std::size_t size) {
	return allocate_counted(size);
}
void *operator new(
	std::size_t size,
	std::align_val_t align) {
	return allocate_counted_aligned(size, static_cast<std::size_t>(align));
}
void *operator new[](
	std::size_t size,
	std::align_val_t align) {
	return allocate_counted_aligned(size, static_cast<std::size_t>(align));
}
void operator delete(
	void *p) noexcept {
	std::free(p);
}
void operator delete[](
	void *p) noexcept {
	std::free(p);
}
void operator delete(
	void *p,
	std::size_t) noexcept {
	std::free(p);
}
void operator delete[](
	void *p,
	std::size_t) noexcept {
	std::free(p);
}
void operator delete(
	void *p,
	std::align_val_t) noexcept {
	std::free(p);
}
void operator delete[](
	void *p,
	std::align_val_t) noexcept {
	std::free(p);
}
void operator delete(
	void *p,
	std::size_t,
	std::align_val_t) noexcept {
	std::free(p);
}
void operator delete[](
	void *p,
	std::size_t,
	std::align_val_t) noexcept {
	std::free(p);
}

namespace {

struct CpuTimes {
	std::uint64_t user_ns{};
	std::uint64_t sys_ns{};
};

[[nodiscard]] std::uint64_t timeval_ns(
	timeval tv) noexcept {
	return static_cast<std::uint64_t>(tv.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(tv.tv_usec) * 1000ULL;
}

[[nodiscard]] CpuTimes cpu_times() noexcept {
	rusage ru{};
	(void)::getrusage(RUSAGE_SELF, &ru);
	return {.user_ns = timeval_ns(ru.ru_utime), .sys_ns = timeval_ns(ru.ru_stime)};
}

[[nodiscard]] std::uint64_t cpu_total(
	CpuTimes t) noexcept {
	return t.user_ns + t.sys_ns;
}

struct AdversarialCase {
	std::string name;
	std::string description;
	std::string raw;
	ParserLimits limits{};
	std::size_t max_body_size = 1024 * 1024;
	std::size_t default_iterations = 100000;
	HttpRejectReason expected_reason = HttpRejectReason::none;
	conflux::http1::ParseStatus expected_parser_status = conflux::http1::ParseStatus::Ok;
	bool decode_chunked_body = false;
	bool incomplete_means_header_timeout = false;
};

struct EvalResult {
	conflux::http1::ParseStatus parser_status{};
	HttpRejectReason reason = HttpRejectReason::none;
	std::size_t parsed_headers{};
	std::size_t consumed_bytes{};
	std::size_t decoded_body_bytes{};
	bool complete = true;
};

struct AdversarialStats {
	std::string_view config;
	std::string_view variant;
	std::string_view description;
	std::string_view reject_reason;
	std::string_view parser_status;
	std::size_t iterations{};
	std::size_t request_bytes{};
	std::size_t consumed_bytes{};
	std::size_t parsed_headers{};
	std::size_t decoded_body_bytes{};
	std::uint64_t total_ns{};
	std::uint64_t cpu_ns{};
	std::size_t sample_count{};
	std::size_t batch{};
	std::uint64_t timer_sample_ns{};
	double timer_overhead_pct{};
	double ns_per_iter{};
	double cpu_ns_per_iter{};
	double bytes_per_iter{};
	double allocations_per_iter{};
	double allocated_bytes_per_iter{};
	double throughput{};
};

[[nodiscard]] char const *parse_status_name(
	conflux::http1::ParseStatus status) noexcept {
	using conflux::http1::ParseStatus;
	switch (status) {
	case ParseStatus::Ok                 : return "ok";
	case ParseStatus::Incomplete         : return "incomplete";
	case ParseStatus::BadRequest         : return "bad_request";
	case ParseStatus::UriTooLong         : return "uri_too_long";
	case ParseStatus::HeaderLineTooLarge : return "header_line_too_large";
	case ParseStatus::HeaderBlockTooLarge: return "header_block_too_large";
	case ParseStatus::TooManyHeaders     : return "too_many_headers";
	}
	return "unknown";
}

[[nodiscard]] ParserLimits default_limits() {
	ParserLimits limits{};
	limits.max_request_line_size = 8192;
	limits.max_header_line_size = 8192;
	limits.max_header_block_size = 65536;
	limits.max_headers = 256;
	limits.max_chunks = 20000;
	return limits;
}

[[nodiscard]] std::string make_header_request(
	std::size_t count) {
	std::string raw = "GET /headers HTTP/1.1\r\nHost: localhost\r\n";
	raw.reserve(64 + count * 32);
	for (std::size_t i = 0; i < count; ++i) {
		raw += std::format("X-Small-{}: v{}\r\n", i, i);
	}
	raw += "\r\n";
	return raw;
}

[[nodiscard]] std::string make_large_near_limit_request(
	ParserLimits const &limits) {
	std::string raw = "GET /large HTTP/1.1\r\nHost: localhost\r\nX-Near-Limit: ";
	auto const suffix = std::string_view{"\r\n\r\n"};
	auto const used_after_request_line = raw.size() - raw.find("\r\n") - 2 + suffix.size();
	if (limits.max_header_block_size > used_after_request_line + 64) {
		raw.append(limits.max_header_block_size - used_after_request_line - 16, 'a');
	} else {
		raw.append(1024, 'a');
	}
	raw += suffix;
	return raw;
}

[[nodiscard]] std::string make_chunked_small_chunks(
	std::size_t chunks) {
	std::string body;
	body.reserve(chunks * 6 + 5);
	for (std::size_t i = 0; i < chunks; ++i) {
		body += "1\r\nx\r\n";
	}
	body += "0\r\n\r\n";
	std::string raw = "POST /upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n";
	raw += body;
	return raw;
}

[[nodiscard]] std::string make_chunked_malformed_late(
	std::size_t good_chunks) {
	std::string raw = "POST /upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n";
	for (std::size_t i = 0; i < good_chunks; ++i) {
		raw += "1\r\nx\r\n";
	}
	raw += "2\r\nzzXX"; // enough bytes to reach DataCrlf and fail on non-CRLF.
	return raw;
}

[[nodiscard]] std::string make_chunked_body_limit_late(
	std::size_t limit) {
	std::string raw = "POST /upload HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n";
	std::size_t remaining = limit + 1;
	while (remaining > 0) {
		auto const n = std::min<std::size_t>(remaining, 64);
		raw += std::format("{:x}\r\n", n);
		raw.append(n, 'b');
		raw += "\r\n";
		remaining -= n;
	}
	raw += "0\r\n\r\n";
	return raw;
}

[[nodiscard]] std::string make_slowloris_headers(
	std::size_t header_bytes) {
	std::string raw = "GET /slow HTTP/1.1\r\nHost: localhost\r\nX-Slow: ";
	raw.append(header_bytes, 's');
	return raw;
}

[[nodiscard]] AdversarialCase make_case(
	std::string_view name) {
	auto limits = default_limits();
	if (name == "many_small_headers"sv) {
		return {
			.name = std::string{name},
			.description = "parse many small valid headers and count header work",
			.raw = make_header_request(192),
			.limits = limits,
			.default_iterations = 80000,
			.expected_reason = HttpRejectReason::none,
			.expected_parser_status = conflux::http1::ParseStatus::Ok};
	}
	if (name == "large_headers_near_limit"sv) {
		return {
			.name = std::string{name},
			.description = "parse one large header block just below aggregate limit",
			.raw = make_large_near_limit_request(limits),
			.limits = limits,
			.default_iterations = 10000,
			.expected_reason = HttpRejectReason::none,
			.expected_parser_status = conflux::http1::ParseStatus::Ok};
	}
	if (name == "invalid_request_line"sv) {
		return {
			.name = std::string{name},
			.description = "reject malformed request line/version",
			.raw = "GET /this-is-not-valid HTTX/9.9\r\nHost: localhost\r\n\r\n",
			.limits = limits,
			.default_iterations = 300000,
			.expected_reason = HttpRejectReason::malformed_request,
			.expected_parser_status = conflux::http1::ParseStatus::BadRequest};
	}
	if (name == "duplicate_content_length"sv) {
		return {
			.name = std::string{name},
			.description = "parse then reject duplicate Content-Length smuggling shape",
			.raw = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello",
			.limits = limits,
			.default_iterations = 120000,
			.expected_reason = HttpRejectReason::duplicate_content_length,
			.expected_parser_status = conflux::http1::ParseStatus::Ok};
	}
	if (name == "te_content_length_ambiguity"sv) {
		return {
			.name = std::string{name},
			.description = "parse then reject Content-Length plus Transfer-Encoding ambiguity",
			.raw =
				"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nTransfer-Encoding: "
				"chunked\r\n\r\n0\r\n\r\n",
			.limits = limits,
			.default_iterations = 120000,
			.expected_reason = HttpRejectReason::content_length_with_transfer_encoding,
			.expected_parser_status = conflux::http1::ParseStatus::Ok};
	}
	if (name == "chunked_small_chunks"sv) {
		return {
			.name = std::string{name},
			.description = "parse/decode many one-byte chunked frames",
			.raw = make_chunked_small_chunks(512),
			.limits = limits,
			.default_iterations = 20000,
			.expected_reason = HttpRejectReason::none,
			.expected_parser_status = conflux::http1::ParseStatus::Ok,
			.decode_chunked_body = true};
	}
	if (name == "chunked_malformed_late"sv) {
		return {
			.name = std::string{name},
			.description = "parse chunked body with late invalid chunk-data CRLF",
			.raw = make_chunked_malformed_late(128),
			.limits = limits,
			.default_iterations = 50000,
			.expected_reason = HttpRejectReason::invalid_chunk,
			.expected_parser_status = conflux::http1::ParseStatus::Ok,
			.decode_chunked_body = true};
	}
	if (name == "body_limit_exceeded_late"sv) {
		return {
			.name = std::string{name},
			.description = "parse/decode chunked body until decoded bytes cross body limit",
			.raw = make_chunked_body_limit_late(4096),
			.limits = limits,
			.max_body_size = 4096,
			.default_iterations = 30000,
			.expected_reason = HttpRejectReason::body_too_large,
			.expected_parser_status = conflux::http1::ParseStatus::Ok,
			.decode_chunked_body = true};
	}
	if (name == "slowloris_headers"sv) {
		return {
			.name = std::string{name},
			.description =
				"scan incomplete slowloris-style growing headers; benchmark records timeout-classification cost",
			.raw = make_slowloris_headers(4096),
			.limits = limits,
			.default_iterations = 120000,
			.expected_reason = HttpRejectReason::header_timeout,
			.expected_parser_status = conflux::http1::ParseStatus::Incomplete,
			.incomplete_means_header_timeout = true};
	}
	throw std::invalid_argument{std::format("unknown HTTP adversarial case: {}", name)};
}

[[nodiscard]] std::vector<std::string_view> all_case_names() {
	return {
		"many_small_headers"sv,
		"large_headers_near_limit"sv,
		"invalid_request_line"sv,
		"duplicate_content_length"sv,
		"te_content_length_ambiguity"sv,
		"chunked_small_chunks"sv,
		"chunked_malformed_late"sv,
		"body_limit_exceeded_late"sv,
		"slowloris_headers"sv,
	};
}

[[nodiscard]] HttpRejectReason parser_reject_reason(
	conflux::http1::ParseStatus status) noexcept {
	using conflux::http1::ParseStatus;
	switch (status) {
	case ParseStatus::Ok                 : return HttpRejectReason::none;
	case ParseStatus::Incomplete         : return HttpRejectReason::none;
	case ParseStatus::BadRequest         : return HttpRejectReason::malformed_request;
	case ParseStatus::UriTooLong         : return HttpRejectReason::request_line_too_large;
	case ParseStatus::HeaderLineTooLarge : return HttpRejectReason::header_line_too_large;
	case ParseStatus::HeaderBlockTooLarge: return HttpRejectReason::header_block_too_large;
	case ParseStatus::TooManyHeaders     : return HttpRejectReason::too_many_headers;
	}
	return HttpRejectReason::malformed_request;
}

[[nodiscard]] bool is_valid_chunked_transfer_encoding(
	std::string_view value) {
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
		value.remove_prefix(1);
	}
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
		value.remove_suffix(1);
	}
	return std::ranges::equal(value, "chunked"sv, [](char a, char b) {
		return static_cast<char>(std::tolower(static_cast<unsigned char>(a))) == b;
	});
}

[[nodiscard]] EvalResult evaluate_case(
	AdversarialCase const &c) {
	conflux::http1::ParsedRequest parsed;
	auto const status = conflux::http1::parse_request(c.raw, c.limits, parsed);
	EvalResult out{.parser_status = status, .consumed_bytes = c.raw.size()};
	if (status != conflux::http1::ParseStatus::Ok) {
		out.complete = status != conflux::http1::ParseStatus::Incomplete;
		out.reason = c.incomplete_means_header_timeout && status == conflux::http1::ParseStatus::Incomplete ?
						 HttpRejectReason::header_timeout :
						 parser_reject_reason(status);
		return out;
	}

	out.parsed_headers = parsed.headers.size();
	out.consumed_bytes = std::min(c.raw.size(), parsed.header_end_offset + 4);
	conflux::http::HttpFieldsView headers{true};
	headers.reserve(parsed.headers.size());
	for (auto const &[name, field_value]: parsed.headers) {
		headers.emplace_back(name, field_value);
	}

	if (parsed.version == "HTTP/1.1") {
		auto const host_count = headers.count("host");
		if (host_count == 0) {
			out.reason = HttpRejectReason::missing_host;
			return out;
		}
		if (host_count > 1) {
			out.reason = HttpRejectReason::duplicate_host;
			return out;
		}
	}

	auto const content_length_count = headers.count("content-length");
	auto const transfer_encoding_count = headers.count("transfer-encoding");
	if (content_length_count != 0 && transfer_encoding_count != 0) {
		out.reason = HttpRejectReason::content_length_with_transfer_encoding;
		return out;
	}
	if (content_length_count > 1) {
		out.reason = HttpRejectReason::duplicate_content_length;
		return out;
	}
	if (transfer_encoding_count > 1) {
		out.reason = HttpRejectReason::invalid_transfer_encoding;
		return out;
	}
	if (content_length_count != 0) {
		auto cl = headers.get("content-length").value_or(std::string_view{});
		std::size_t content_length{};
		auto const *cl_end = cl.data() + cl.size();
		auto [ptr, ec] = std::from_chars(cl.data(), cl_end, content_length);
		if (ec != std::errc{} || ptr != cl_end) {
			out.reason = HttpRejectReason::malformed_content_length;
			return out;
		}
		if (content_length > c.max_body_size) {
			out.reason = HttpRejectReason::body_too_large;
			return out;
		}
		if (c.raw.size() - (parsed.header_end_offset + 4) < content_length) {
			out.complete = false;
			return out;
		}
		out.consumed_bytes = parsed.header_end_offset + 4 + content_length;
		return out;
	}
	if (transfer_encoding_count != 0) {
		auto const te = headers.get("transfer-encoding").value_or(std::string_view{});
		if (!is_valid_chunked_transfer_encoding(te)) {
			out.reason = HttpRejectReason::unsupported_transfer_encoding;
			return out;
		}
		if (c.decode_chunked_body) {
			conflux::http::ChunkedDecodeState state;
			auto const body_start = parsed.header_end_offset + 4;
			auto const rc = conflux::http::decode_chunked_incremental(
				c.raw,
				body_start,
				c.max_body_size,
				c.limits.max_chunks,
				state);
			if (rc == 0) {
				out.complete = false;
				out.decoded_body_bytes = state.body.size();
				return out;
			}
			if (rc == -1) {
				out.reason = HttpRejectReason::invalid_chunk;
				out.decoded_body_bytes = state.body.size();
				out.consumed_bytes = c.raw.size();
				return out;
			}
			if (rc == -2) {
				out.reason = HttpRejectReason::body_too_large;
				out.decoded_body_bytes = state.body.size();
				out.consumed_bytes = c.raw.size();
				return out;
			}
			out.decoded_body_bytes = state.body.size();
			out.consumed_bytes = body_start + static_cast<std::size_t>(rc);
		}
	}
	return out;
}

[[nodiscard]] EvalResult checked_eval(
	AdversarialCase const &c) {
	auto out = evaluate_case(c);
	if (out.parser_status != c.expected_parser_status) {
		throw std::runtime_error{std::format(
			"{} parser status mismatch: got {}, expected {}",
			c.name,
			parse_status_name(out.parser_status),
			parse_status_name(c.expected_parser_status))};
	}
	if (out.reason != c.expected_reason) {
		throw std::runtime_error{std::format(
			"{} reject reason mismatch: got {}, expected {}",
			c.name,
			reject_reason_code(out.reason),
			reject_reason_code(c.expected_reason))};
	}
	return out;
}

[[nodiscard]] AdversarialStats bench_case(
	AdversarialCase const &c,
	BenchArgs const &args,
	std::size_t iterations) {
	BenchSamplePlan const plan = bench_sample_plan(iterations, args.warmup, args.samples, args.batch);
	auto run_once = [&] {
		auto out = checked_eval(c);
		g_sink.fetch_add(out.parsed_headers + out.consumed_bytes + out.decoded_body_bytes, std::memory_order_relaxed);
		return out;
	};
	for (std::size_t i = 0; i < plan.warmup_iterations; ++i) {
		(void)run_once();
	}

	reset_allocation_counters();
	g_count_allocations.store(true, std::memory_order_relaxed);
	auto const cpu0 = cpu_times();
	EvalResult last{};
	std::vector<std::uint64_t> samples;
	samples.reserve(plan.samples);
	std::uint64_t total_ns = 0;
	for (std::size_t sample = 0; sample < plan.samples; ++sample) {
		auto const t0 = bench_now_ns();
		for (std::size_t i = 0; i < plan.batch; ++i) {
			last = run_once();
		}
		auto const elapsed = bench_now_ns() - t0;
		total_ns += elapsed;
		samples.push_back(elapsed);
	}
	auto const cpu1 = cpu_times();
	g_count_allocations.store(false, std::memory_order_relaxed);
	std::ranges::sort(samples);
	auto const total_allocs = g_allocations.load(std::memory_order_relaxed);
	auto const total_bytes = g_allocated_bytes.load(std::memory_order_relaxed);
	auto const cpu_ns = cpu_total(cpu1) - cpu_total(cpu0);
	auto const iter_d = static_cast<double>(plan.iterations);
	return {
		.config = c.name,
		.variant = c.name,
		.description = c.description,
		.reject_reason = reject_reason_code(last.reason),
		.parser_status = parse_status_name(last.parser_status),
		.iterations = plan.iterations,
		.request_bytes = c.raw.size(),
		.consumed_bytes = last.consumed_bytes,
		.parsed_headers = last.parsed_headers,
		.decoded_body_bytes = last.decoded_body_bytes,
		.total_ns = total_ns,
		.cpu_ns = cpu_ns,
		.sample_count = plan.samples,
		.batch = plan.batch,
		.timer_sample_ns = plan.timer_sample_ns,
		.timer_overhead_pct = bench_timer_overhead_percent(plan, total_ns),
		.ns_per_iter = static_cast<double>(samples[plan.samples / 2]) / static_cast<double>(plan.batch),
		.cpu_ns_per_iter = static_cast<double>(cpu_ns) / iter_d,
		.bytes_per_iter = static_cast<double>(c.raw.size()),
		.allocations_per_iter = static_cast<double>(total_allocs) / iter_d,
		.allocated_bytes_per_iter = static_cast<double>(total_bytes) / iter_d,
		.throughput = iter_d * 1e9 / static_cast<double>(total_ns)};
}

void print_stats(
	AdversarialStats const &s,
	bool json_out,
	bool first) {
	if (json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"cpu_"
			"ns\":{},\"cpu_ns_per_iter\":{:.2f},\"request_bytes\":{},\"consumed_bytes\":{},\"parsed_headers\":{},"
			"\"decoded_body_bytes\":{},\"reject_reason\":\"{}\",\"parser_status\":\"{}\",\"sample_count\":{},\"batch\":"
			"{},"
			"\"timer_sample_ns\":{},\"timer_overhead_pct\":{:.4f},\"allocations_per_iter\":{:."
			"4f},\"allocated_bytes_per_iter\":{:.2f},\"label\":\"micro/user-space-adversarial\"}}",
			s.config,
			s.variant,
			s.iterations,
			s.total_ns,
			s.ns_per_iter,
			s.cpu_ns,
			s.cpu_ns_per_iter,
			s.request_bytes,
			s.consumed_bytes,
			s.parsed_headers,
			s.decoded_body_bytes,
			s.reject_reason,
			s.parser_status,
			s.sample_count,
			s.batch,
			s.timer_sample_ns,
			s.timer_overhead_pct,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter);
		(void)first;
		return;
	}
	if (first) {
		std::println(
			"{:<30} {:>10} {:>11} {:>10} {:>8} {:>10}  {}",
			"case",
			"iters",
			"ns/iter",
			"cpu/iter",
			"headers",
			"reason",
			"description");
	}
	std::println(
		"{:<30} {:>10} {:>11.2f} {:>10.2f} {:>8} {:>10}  [{}×{} timer≈{:.2f}%] {}",
		s.variant,
		s.iterations,
		s.ns_per_iter,
		s.cpu_ns_per_iter,
		s.parsed_headers,
		s.reject_reason,
		s.sample_count,
		s.batch,
		s.timer_overhead_pct,
		s.description);
}

[[nodiscard]] std::string_view parse_case_name(
	std::span<char *> args) {
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const arg{args[i]};
		if (arg == "--case"sv && i + 1 < args.size()) {
			return args[i + 1];
		}
	}
	return "many_small_headers"sv;
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

[[nodiscard]] std::vector<AdversarialCase> selected_cases(
	std::span<char *> args) {
	if (has_flag(args, "--all-cases"sv)) {
		std::vector<AdversarialCase> out;
		for (auto name: all_case_names()) {
			out.push_back(make_case(name));
		}
		return out;
	}
	return {make_case(parse_case_name(args))};
}

void print_list() {
	for (auto name: all_case_names()) {
		auto c = make_case(name);
		std::println("{:<30} {}", c.name, c.description);
	}
}

void print_usage() {
	std::println(
		"Usage: conflux_http_adversarial_bench [--case NAME|--all-cases] [--iterations N] [--warmup N] [--json] "
		"[--list]");
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"http_adversarial","parser":"standard","configs":[{"name":"many_small_headers","extra":{"kind":"micro/user-space-adversarial","case":"many small headers"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":8,"args":["--case","many_small_headers","--config-name","many_small_headers","--iterations","0","--warmup","0"]},{"name":"large_headers_near_limit","extra":{"kind":"micro/user-space-adversarial","case":"large headers near limit"},"target_ms":500,"max_iterations":500000,"calibration_iterations":4,"args":["--case","large_headers_near_limit","--config-name","large_headers_near_limit","--iterations","0","--warmup","0"]},{"name":"invalid_request_line","extra":{"kind":"micro/user-space-adversarial","case":"invalid request line"},"target_ms":500,"max_iterations":5000000,"calibration_iterations":16,"args":["--case","invalid_request_line","--config-name","invalid_request_line","--iterations","0","--warmup","0"]},{"name":"duplicate_content_length","extra":{"kind":"micro/user-space-adversarial","case":"duplicate Content-Length"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":8,"args":["--case","duplicate_content_length","--config-name","duplicate_content_length","--iterations","0","--warmup","0"]},{"name":"te_content_length_ambiguity","extra":{"kind":"micro/user-space-adversarial","case":"Transfer-Encoding ambiguity"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":8,"args":["--case","te_content_length_ambiguity","--config-name","te_content_length_ambiguity","--iterations","0","--warmup","0"]},{"name":"chunked_small_chunks","extra":{"kind":"micro/user-space-adversarial","case":"chunked small chunks"},"target_ms":500,"max_iterations":500000,"calibration_iterations":4,"args":["--case","chunked_small_chunks","--config-name","chunked_small_chunks","--iterations","0","--warmup","0"]},{"name":"chunked_malformed_late","extra":{"kind":"micro/user-space-adversarial","case":"chunked malformed late"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":4,"args":["--case","chunked_malformed_late","--config-name","chunked_malformed_late","--iterations","0","--warmup","0"]},{"name":"body_limit_exceeded_late","extra":{"kind":"micro/user-space-adversarial","case":"body limit exceeded late"},"target_ms":500,"max_iterations":1000000,"calibration_iterations":4,"args":["--case","body_limit_exceeded_late","--config-name","body_limit_exceeded_late","--iterations","0","--warmup","0"]},{"name":"slowloris_headers","extra":{"kind":"micro/user-space-adversarial","case":"slowloris headers"},"target_ms":500,"max_iterations":2000000,"calibration_iterations":8,"args":["--case","slowloris_headers","--config-name","slowloris_headers","--iterations","0","--warmup","0"]}]})");
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
		auto const cases = selected_cases(args);
		bool first = true;
		for (auto const &c: cases) {
			auto const iterations = cfg.iterations == 0 ? c.default_iterations : cfg.iterations;
			auto const stats = bench_case(c, cfg, iterations);
			print_stats(stats, cfg.json_out, first);
			first = false;
		}
		std::println(std::cerr, "sink={}", g_sink.load(std::memory_order_relaxed));
		return 0;
	} catch (std::exception const &ex) {
		std::println(std::cerr, "conflux_http_adversarial_bench: {}", ex.what());
		print_usage();
		return 1;
	}
}
