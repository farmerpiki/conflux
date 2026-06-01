// db_protocol_synthetic_bench — PostgreSQL user-space decode/protocol floor.
//
// Live db_coro/db_pipeline rows include PostgreSQL server execution, socket I/O,
// libpq polling, and kernel wakeups. This bench keeps the data shape similar but
// removes the server/socket path so row extraction and protocol framing costs are
// visible as micro/user-space rows.

#include <libpq-fe.h>

import std;
import bench_common;

using namespace std::string_view_literals;

namespace {

inline std::atomic<std::uint64_t> sink{};
using FakeResult = std::unique_ptr<PGresult, decltype(&::PQclear)>;

struct Config {
	std::size_t iterations = 2000;
	std::size_t warmup = 100;
	std::size_t rows = 100;
	std::size_t payload_bytes = 24;
	bool json_out = false;
	std::string config_name;
	BenchArgs bench;
};

[[nodiscard]] Config parse_args(
	std::span<char *> args) {
	Config cfg;
	auto base = bench_parse_args(args);
	if (!base.iterations_explicit || base.iterations > 0) {
		cfg.iterations = base.iterations;
	}
	cfg.warmup = base.warmup;
	cfg.json_out = base.json_out;
	cfg.config_name = std::move(base.config_name);
	cfg.bench = std::move(base);
	for (std::size_t i = 1; i < args.size(); ++i) {
		std::string_view const a = args[i];
		if (a == "--rows" && i + 1 < args.size()) {
			cfg.rows = bench_parse_sz(args[++i]);
		} else if (a == "--payload-bytes" && i + 1 < args.size()) {
			cfg.payload_bytes = bench_parse_sz(args[++i]);
		}
	}
	cfg.rows = std::max<std::size_t>(1, cfg.rows);
	cfg.payload_bytes = std::max<std::size_t>(1, cfg.payload_bytes);
	if (cfg.config_name.empty()) {
		cfg.config_name = std::format("rows_{}_payload_{}", cfg.rows, cfg.payload_bytes);
	}
	return cfg;
}

[[nodiscard]] std::string make_payload(
	std::size_t row,
	std::size_t bytes) {
	std::string out;
	out.reserve(bytes);
	while (out.size() < bytes) {
		out += std::format("row{}_", row);
	}
	out.resize(bytes);
	return out;
}

[[nodiscard]] FakeResult make_fake_result(
	std::size_t rows,
	std::size_t payload_bytes) {
	PGresult *raw = ::PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
	if (raw == nullptr) {
		throw std::runtime_error{"PQmakeEmptyPGresult failed"};
	}
	auto holder = std::unique_ptr<PGresult, decltype(&::PQclear)>{raw, &::PQclear};

	std::array<char, 3> id_name{'i', 'd', '\0'};
	std::array<char, 6> label_name{'l', 'a', 'b', 'e', 'l', '\0'};
	std::array<PGresAttDesc, 2> attrs{};
	attrs[0].name = id_name.data();
	attrs[0].format = 0;
	attrs[0].typid = 20;
	attrs[0].typlen = 8;
	attrs[1].name = label_name.data();
	attrs[1].format = 0;
	attrs[1].typid = 25;
	attrs[1].typlen = -1;
	if (::PQsetResultAttrs(raw, static_cast<int>(attrs.size()), attrs.data()) == 0) {
		throw std::runtime_error{"PQsetResultAttrs failed"};
	}

	for (std::size_t r = 0; r < rows; ++r) {
		std::string id = std::to_string(r + 1);
		std::string payload = make_payload(r, payload_bytes);
		if (::PQsetvalue(raw, static_cast<int>(r), 0, id.data(), static_cast<int>(id.size())) == 0) {
			throw std::runtime_error{"PQsetvalue id failed"};
		}
		if (::PQsetvalue(raw, static_cast<int>(r), 1, payload.data(), static_cast<int>(payload.size())) == 0) {
			throw std::runtime_error{"PQsetvalue payload failed"};
		}
	}
	return holder;
}

void consume_result_text(
	FakeResult const &rs) {
	std::uint64_t acc = 0;
	int const rows = ::PQntuples(rs.get());
	for (int r = 0; r < rows; ++r) {
		std::string_view id{::PQgetvalue(rs.get(), r, 0), static_cast<std::size_t>(::PQgetlength(rs.get(), r, 0))};
		std::uint64_t v = 0;
		auto const *first = id.data();
		auto const *last = first + id.size();
		auto [parsed, ec] = std::from_chars(first, last, v);
		if (ec != std::errc{} || parsed != last) [[unlikely]] {
			throw std::runtime_error{"libpq result int parse failed"};
		}
		acc += v;
		acc += static_cast<std::uint64_t>(::PQgetlength(rs.get(), r, 1));
	}
	sink.fetch_add(acc, std::memory_order_relaxed);
}

void append_u16(
	std::vector<std::byte> &out,
	std::uint16_t v) {
	out.push_back(static_cast<std::byte>((v >> 8U) & 0xffU));
	out.push_back(static_cast<std::byte>(v & 0xffU));
}

void append_u32(
	std::vector<std::byte> &out,
	std::uint32_t v) {
	out.push_back(static_cast<std::byte>((v >> 24U) & 0xffU));
	out.push_back(static_cast<std::byte>((v >> 16U) & 0xffU));
	out.push_back(static_cast<std::byte>((v >> 8U) & 0xffU));
	out.push_back(static_cast<std::byte>(v & 0xffU));
}

void append_i64_be(
	std::vector<std::byte> &out,
	std::int64_t v) {
	auto u = static_cast<std::uint64_t>(v);
	for (int shift = 56; shift >= 0; shift -= 8) {
		out.push_back(static_cast<std::byte>((u >> static_cast<unsigned>(shift)) & 0xffU));
	}
}

[[nodiscard]] std::uint16_t read_u16(
	std::byte const *p) noexcept {
	return static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) << 8U)
		| static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1])));
}

[[nodiscard]] std::uint32_t read_u32(
	std::byte const *p) noexcept {
	return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24U)
		 | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16U)
		 | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8U)
		 | static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
}

[[nodiscard]] std::int64_t read_i64_be(
	std::byte const *p) noexcept {
	std::uint64_t v = 0;
	for (std::size_t i = 0; i < 8; ++i) {
		v = (v << 8U) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
	}
	return static_cast<std::int64_t>(v);
}

[[nodiscard]] std::vector<std::byte> make_wire_rows_text(
	std::size_t rows,
	std::size_t payload_bytes) {
	std::vector<std::byte> out;
	out.reserve(rows * (1 + 4 + 2 + 4 + 20 + 4 + payload_bytes));
	for (std::size_t r = 0; r < rows; ++r) {
		std::string id = std::to_string(r + 1);
		std::string payload = make_payload(r, payload_bytes);
		std::uint32_t const msg_len = static_cast<std::uint32_t>(4 + 2 + 4 + id.size() + 4 + payload.size());
		out.push_back(static_cast<std::byte>('D'));
		append_u32(out, msg_len);
		append_u16(out, 2);
		append_u32(out, static_cast<std::uint32_t>(id.size()));
		std::ranges::transform(id, std::back_inserter(out), [](char c) { return static_cast<std::byte>(c); });
		append_u32(out, static_cast<std::uint32_t>(payload.size()));
		std::ranges::transform(payload, std::back_inserter(out), [](char c) { return static_cast<std::byte>(c); });
	}
	return out;
}

[[nodiscard]] std::vector<std::byte> make_wire_rows_binary(
	std::size_t rows,
	std::size_t payload_bytes) {
	std::vector<std::byte> out;
	out.reserve(rows * (1 + 4 + 2 + 4 + 8 + 4 + payload_bytes));
	for (std::size_t r = 0; r < rows; ++r) {
		std::string payload = make_payload(r, payload_bytes);
		std::uint32_t const msg_len = static_cast<std::uint32_t>(4 + 2 + 4 + 8 + 4 + payload.size());
		out.push_back(static_cast<std::byte>('D'));
		append_u32(out, msg_len);
		append_u16(out, 2);
		append_u32(out, 8);
		append_i64_be(out, static_cast<std::int64_t>(r + 1));
		append_u32(out, static_cast<std::uint32_t>(payload.size()));
		std::ranges::transform(payload, std::back_inserter(out), [](char c) { return static_cast<std::byte>(c); });
	}
	return out;
}

void consume_wire_text(
	std::span<std::byte const> data) {
	std::uint64_t acc = 0;
	auto const *p = data.data();
	auto const *end = data.data() + data.size();
	while (p < end) {
		if (*p++ != static_cast<std::byte>('D')) [[unlikely]] {
			throw std::runtime_error{"bad DataRow tag"};
		}
		auto const len = read_u32(p);
		p += 4;
		auto const *msg_end = p + len - 4;
		auto const cols = read_u16(p);
		p += 2;
		for (std::uint16_t c = 0; c < cols; ++c) {
			auto const field_len = read_u32(p);
			p += 4;
			if (field_len == 0xffffffffU) {
				continue;
			}
			std::string_view sv{reinterpret_cast<char const *>(p), field_len};
			if (c == 0) {
				std::uint64_t v = 0;
				auto const *first = sv.data();
				auto const *last = first + sv.size();
				auto [parsed, ec] = std::from_chars(first, last, v);
				if (ec != std::errc{} || parsed != last) [[unlikely]] {
					throw std::runtime_error{"wire int parse failed"};
				}
				acc += v;
			} else {
				acc += static_cast<std::uint64_t>(sv.size());
			}
			p += field_len;
		}
		if (p != msg_end) [[unlikely]] {
			throw std::runtime_error{"bad DataRow length"};
		}
	}
	sink.fetch_add(acc, std::memory_order_relaxed);
}

void consume_wire_binary(
	std::span<std::byte const> data) {
	std::uint64_t acc = 0;
	auto const *p = data.data();
	auto const *end = data.data() + data.size();
	while (p < end) {
		if (*p++ != static_cast<std::byte>('D')) [[unlikely]] {
			throw std::runtime_error{"bad DataRow tag"};
		}
		auto const len = read_u32(p);
		p += 4;
		auto const *msg_end = p + len - 4;
		auto const cols = read_u16(p);
		p += 2;
		for (std::uint16_t c = 0; c < cols; ++c) {
			auto const field_len = read_u32(p);
			p += 4;
			if (field_len == 0xffffffffU) {
				continue;
			}
			if (c == 0) {
				if (field_len != 8) [[unlikely]] {
					throw std::runtime_error{"wire int8 length mismatch"};
				}
				acc += static_cast<std::uint64_t>(read_i64_be(p));
			} else {
				acc += field_len;
			}
			p += field_len;
		}
		if (p != msg_end) [[unlikely]] {
			throw std::runtime_error{"bad DataRow length"};
		}
	}
	sink.fetch_add(acc, std::memory_order_relaxed);
}

template<class F>
[[nodiscard]] BenchStats time_loop(
	Config const &cfg,
	F &&fn) {
	BenchSamplePlan const plan = bench_sample_plan(cfg.iterations, cfg.warmup, cfg.bench.samples, cfg.bench.batch);
	for (std::size_t i = 0; i < plan.warmup_samples; ++i) {
		for (std::size_t j = 0; j < plan.batch; ++j) {
			fn();
		}
	}
	std::vector<std::uint64_t> samples;
	samples.reserve(plan.samples);
	std::uint64_t total_ns = 0;
	for (std::size_t i = 0; i < plan.samples; ++i) {
		std::uint64_t const t0 = bench_now_ns();
		for (std::size_t j = 0; j < plan.batch; ++j) {
			fn();
		}
		std::uint64_t const elapsed = bench_now_ns() - t0;
		total_ns += elapsed;
		samples.push_back(elapsed);
	}
	std::ranges::sort(samples);
	BenchStats stats{
		.config = cfg.config_name,
		.iterations = plan.iterations * cfg.rows,
		.total_ns = total_ns,
		.ns_per_iter = static_cast<double>(samples[plan.samples / 2])
					 / static_cast<double>(std::max<std::size_t>(1, plan.batch * cfg.rows)),
		.sample_count = plan.samples,
		.batch = plan.batch * cfg.rows,
		.timer_sample_ns = plan.timer_sample_ns,
	};
	stats.timer_overhead_pct = bench_timer_overhead_percent(plan, stats.total_ns);
	return stats;
}

void print_row(
	Config const &cfg,
	std::string_view variant,
	BenchStats stats,
	std::size_t bytes_per_scan,
	bool &first) {
	stats.variant = variant;
	if (cfg.json_out) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},\"label\":"
			"\"micro/"
			"user-space\",\"sample_count\":{},\"batch\":{},\"timer_sample_ns\":{},\"timer_overhead_pct\":{:.4f},"
			"\"rows_per_scan\":{},\"payload_bytes\":{},\"bytes_per_scan\":{},\"sink\":{}}}",
			cfg.config_name,
			variant,
			stats.iterations,
			stats.total_ns,
			stats.ns_per_iter,
			stats.sample_count,
			stats.batch,
			stats.timer_sample_ns,
			stats.timer_overhead_pct,
			cfg.rows,
			cfg.payload_bytes,
			bytes_per_scan,
			sink.load(std::memory_order_relaxed));
	} else {
		if (first) {
			std::println("db_protocol_synthetic [{}]", cfg.config_name);
			first = false;
		}
		std::println(
			"{:<28} {:>10} rows {:>9.2f} ns/row  [{}×{} samples, timer≈{:.2f}%] bytes/scan={} label=micro/user-space",
			variant,
			stats.iterations,
			stats.ns_per_iter,
			stats.sample_count,
			stats.batch,
			stats.timer_overhead_pct,
			bytes_per_scan);
	}
}

} // namespace

int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"db_protocol_synthetic","parser":"standard","configs":[{"name":"rows_16_payload_24","extra":{"rows":16,"payload_bytes":24},"target_ms":500,"max_iterations":200000,"calibration_iterations":8,"args":["--rows","16","--payload-bytes","24","--config-name","rows_16_payload_24","--iterations","0","--warmup","200"]},{"name":"rows_128_payload_24","extra":{"rows":128,"payload_bytes":24},"target_ms":500,"max_iterations":100000,"calibration_iterations":8,"args":["--rows","128","--payload-bytes","24","--config-name","rows_128_payload_24","--iterations","0","--warmup","100"]},{"name":"rows_128_payload_256","extra":{"rows":128,"payload_bytes":256},"target_ms":500,"max_iterations":50000,"calibration_iterations":4,"args":["--rows","128","--payload-bytes","256","--config-name","rows_128_payload_256","--iterations","0","--warmup","50"]}]})");

	auto cfg = parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	auto fake = make_fake_result(cfg.rows, cfg.payload_bytes);
	auto wire_text = make_wire_rows_text(cfg.rows, cfg.payload_bytes);
	auto wire_binary = make_wire_rows_binary(cfg.rows, cfg.payload_bytes);

	bool first = true;
	auto const result_ns = time_loop(cfg, [&] { consume_result_text(fake); });
	print_row(cfg, "libpq_result_row_decode_text"sv, result_ns, wire_text.size(), first);
	auto const wire_text_ns = time_loop(cfg, [&] { consume_wire_text(wire_text); });
	print_row(cfg, "wire_data_row_scan_text"sv, wire_text_ns, wire_text.size(), first);
	auto const wire_binary_ns = time_loop(cfg, [&] { consume_wire_binary(wire_binary); });
	print_row(cfg, "wire_data_row_scan_binary"sv, wire_binary_ns, wire_binary.size(), first);
	if (!cfg.json_out) {
		std::println("sink={}", sink.load(std::memory_order_relaxed));
	}
	return 0;
}
