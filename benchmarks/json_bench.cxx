#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <liburing.h>
#include <netinet/in.h>
#include <new>
#include <sys/socket.h>
#include <unistd.h>

#ifndef __has_feature
	#define __has_feature(x) 0
#endif

import std;
import conflux.types;
import conflux.work;
import conflux.file_io;
import conflux.socket_io;
import conflux.socket_io.coro;
import conflux.socket_io.blocking;
import conflux.json;
import conflux.utils;
import bench_common;

using namespace conflux::json;

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_alloc_count{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};

} // namespace

#if !defined(__SANITIZE_THREAD__) && !__has_feature(thread_sanitizer)
void *operator new(
	std::size_t size) {
	if (void *p = std::malloc(size)) {
		if (g_count_allocations.load(std::memory_order_relaxed)) {
			g_alloc_count.fetch_add(1, std::memory_order_relaxed);
			g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
		}
		return p;
	}
	throw std::bad_alloc{};
}
void *operator new[](
	std::size_t size) {
	return ::operator new(size);
}
// Keep delete hooks out of call sites so GCC sees the replacement new/free pair correctly.
[[gnu::noinline]] void operator delete(
	void *p) noexcept {
	std::free(p);
}
[[gnu::noinline]] void operator delete[](
	void *p) noexcept {
	::operator delete(p);
}
[[gnu::noinline]] void operator delete(
	void *p,
	std::size_t) noexcept {
	::operator delete(p);
}
[[gnu::noinline]] void operator delete[](
	void *p,
	std::size_t) noexcept {
	::operator delete(p);
}

#endif

namespace {

struct AllocBenchStats {
	BenchStats timing;
	double allocations_per_iter{};
	double allocated_bytes_per_iter{};
};

template<typename F>
BenchStats measure(F &&fn, std::size_t warmup, std::size_t iters, std::size_t batch = 1, std::size_t bytes = 0);
template<typename F>
AllocBenchStats
measure_alloc(F &&fn, std::size_t warmup, std::size_t iters, std::size_t batch = 1, std::size_t bytes = 0);

bool g_csv = false;
bool g_first_row = true;
std::vector<std::string> g_filters;
BenchArgs g_args;

[[nodiscard]] BenchSamplePlan make_plan(
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch = 1) {
	return bench_sample_plan(g_args, iters, warmup, batch);
}

template<typename F>
BenchStats measure(
	F &&fn,
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch,
	std::size_t bytes) {
	return bench_measure_batched(std::forward<F>(fn), make_plan(warmup, iters, batch), bytes);
}

template<typename F>
AllocBenchStats measure_alloc(
	F &&fn,
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch,
	std::size_t bytes) {
	BenchSamplePlan const plan = make_plan(warmup, iters, batch);
	for (std::size_t i = 0; i < plan.warmup_samples; ++i) {
		for (std::size_t j = 0; j < plan.batch; ++j) {
			fn();
		}
	}
	std::vector<std::uint64_t> samples;
	samples.reserve(plan.samples);
	std::uint64_t total = 0;
	std::uint64_t total_allocs = 0;
	std::uint64_t total_bytes = 0;
	for (std::size_t i = 0; i < plan.samples; ++i) {
		g_alloc_count.store(0, std::memory_order_relaxed);
		g_alloc_bytes.store(0, std::memory_order_relaxed);
		g_count_allocations.store(true, std::memory_order_relaxed);
		std::uint64_t const t0 = bench_now_ns();
		for (std::size_t j = 0; j < plan.batch; ++j) {
			fn();
		}
		std::uint64_t const elapsed = bench_now_ns() - t0;
		g_count_allocations.store(false, std::memory_order_relaxed);
		total += elapsed;
		total_allocs += g_alloc_count.load(std::memory_order_relaxed);
		total_bytes += g_alloc_bytes.load(std::memory_order_relaxed);
		samples.push_back(elapsed);
	}
	std::ranges::sort(samples);
	auto percentile_ns = [&](std::size_t numerator, std::size_t denominator) {
		std::size_t const last = samples.size() - 1;
		std::size_t const index = (last * numerator + denominator - 1) / denominator;
		return static_cast<double>(samples[index]) / static_cast<double>(plan.batch);
	};
	double const best_ns = static_cast<double>(samples.front()) / static_cast<double>(plan.batch);
	double const p10_ns = percentile_ns(10, 100);
	double const p50_ns = static_cast<double>(samples[plan.samples / 2]) / static_cast<double>(plan.batch);
	double const p99_ns = percentile_ns(99, 100);
	double const mbs =
		(bytes > 0 && p50_ns > 0.0) ? static_cast<double>(bytes) / (p50_ns / 1e9) / (1024.0 * 1024.0) : 0.0;
	double const denom = static_cast<double>(plan.iterations);
	BenchStats timing{
		.iterations = plan.iterations,
		.total_ns = total,
		.ns_per_iter = p50_ns,
		.best_ns_per_iter = best_ns,
		.p10_ns_per_iter = p10_ns,
		.p50_ns_per_iter = p50_ns,
		.p99_ns_per_iter = p99_ns,
		.throughput = mbs,
	};
	bench_apply_sample_plan(timing, plan);
	return {
		.timing = timing,
		.allocations_per_iter = static_cast<double>(total_allocs) / denom,
		.allocated_bytes_per_iter = static_cast<double>(total_bytes) / denom,
	};
}

void print_row(std::string_view name, BenchStats s);
void print_alloc_row(std::string_view name, AllocBenchStats s);

[[nodiscard]] bool should_run(
	std::string_view name) {
	return bench_matches_filter(std::span<std::string const>{g_filters}, name);
}

template<class T>
void bench_consume(
	T const &value) noexcept {
	asm volatile("" : : "g"(&value) : "memory");
}

template<class F>
void run_row(
	std::string_view name,
	F &&fn) {
	if (!should_run(name)) {
		return;
	}
	print_row(name, fn());
}

template<class F>
void run_alloc_row(
	std::string_view name,
	F &&fn) {
	if (!should_run(name)) {
		return;
	}
	print_alloc_row(name, fn());
}

void print_row(
	std::string_view name,
	BenchStats s) {
	s.variant = name;
	if (g_csv) {
		bench_print(s, true, g_first_row);
		g_first_row = false;
	} else if (s.throughput > 0.0) {
		print(
			"[json-bench] {:<40} {:>10.1f} ns  {:>8.1f} MB/s  [{}×{} timer≈{:.2f}%]\n",
			name,
			s.ns_per_iter,
			s.throughput,
			s.sample_count,
			s.batch,
			s.timer_overhead_pct);
	} else {
		print(
			"[json-bench] {:<40} {:>10.1f} ns  [{}×{} timer≈{:.2f}%]\n",
			name,
			s.ns_per_iter,
			s.sample_count,
			s.batch,
			s.timer_overhead_pct);
	}
}
void print_alloc_row(
	std::string_view name,
	AllocBenchStats s) {
	s.timing.variant = name;
	if (g_csv) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"best_ns_per_iter\":{:.2f},\"p10_ns_per_iter\":{:.2f},\"p50_ns_per_iter\":{:.2f},"
			"\"p99_ns_per_iter\":{:.2f},\"sample_count\":{},\"batch\":{},\"timer_sample_ns\":{},"
			"\"timer_overhead_pct\":{:.4f},"
			"\"allocations_per_iter\":{:.2f},\"allocated_bytes_per_iter\":{:.2f}}}",
			s.timing.config,
			s.timing.variant,
			s.timing.iterations,
			s.timing.total_ns,
			s.timing.ns_per_iter,
			s.timing.best_ns_per_iter,
			s.timing.p10_ns_per_iter,
			s.timing.p50_ns_per_iter,
			s.timing.p99_ns_per_iter,
			s.timing.sample_count,
			s.timing.batch,
			s.timing.timer_sample_ns,
			s.timing.timer_overhead_pct,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter);
		g_first_row = false;
	} else if (s.timing.throughput > 0.0) {
		print(
			"[json-bench] {:<40} {:>10.1f} ns  {:>8.1f} MB/s  {:>6.2f} allocs  {:>8.1f} B  [{}×{} timer≈{:.2f}%]\n",
			name,
			s.timing.ns_per_iter,
			s.timing.throughput,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter,
			s.timing.sample_count,
			s.timing.batch,
			s.timing.timer_overhead_pct);
	} else {
		print(
			"[json-bench] {:<40} {:>10.1f} ns  {:>6.2f} allocs  {:>8.1f} B  [{}×{} timer≈{:.2f}%]\n",
			name,
			s.timing.ns_per_iter,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter,
			s.timing.sample_count,
			s.timing.batch,
			s.timing.timer_overhead_pct);
	}
}
// ---------------------------------------------------------------------------
// Corpus builders
// ---------------------------------------------------------------------------

// Typical config corpus: ~4 KB flat object with S/number/bool values.
std::string make_config_corpus() {
	std::string out;
	out.reserve(4096);
	out += '{';
	for (int i = 0; i < 64; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(
			R"("key_{}":{{"value":{},"label":"item_{}","active":{}}})",
			i,
			i * 17,
			i,
			(i % 2 == 0) ? "true" : "false");
	}
	out += '}';
	return out;
}
// Struct-decode corpus: A of objects with SV-compatible fields.
std::string make_decode_corpus() {
	std::string out;
	out.reserve(8192);
	out += '[';
	for (int i = 0; i < 200; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(R"({{"id":{},"name":"user_{}","score":{}}})", i, i, i * 3.14);
	}
	out += ']';
	return out;
}
// Lookup corpus: object with 1024 members.
std::string make_lookup_corpus() {
	std::string out;
	out.reserve(32768);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(R"("member_{}":{},)", i, i); // extra comma intentional — remove after
		out.pop_back();
	}
	out += '}';
	return out;
}
// Array traversal corpus: A of 10000 numbers.
std::string make_array_corpus() {
	std::string out;
	out.reserve(65536);
	out += '[';
	for (int i = 0; i < 10000; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::to_string(i);
	}
	out += ']';
	return out;
}
// Large corpus for parse throughput gate: ~1 MB nested structure.
std::string make_large_corpus() {
	std::string out;
	out.reserve(1024UZ * 1024UZ);
	out += '[';
	for (int i = 0; i < 2000; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(
			R"({{"id":{},"name":"entry_{}","tags":["alpha","beta","gamma"],"meta":{{"score":{},"active":{}}}}})",
			i,
			i,
			i * 1.5,
			(i % 2 == 0) ? "true" : "false");
	}
	out += ']';
	return out;
}
// R0 — long-S-heavy corpus: 32 elements of 32 KiB ASCII payload, no
// escapes. Exercises memcpy-free zero-copy std::string slice + the SIMD scan_str
// fast path on long unescaped runs.
std::string make_long_strings_corpus() {
	std::string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += '[';
	constexpr int kElems = 32;
	constexpr int kLen = 32 * 1024;
	for (int i = 0; i < kElems; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += '"';
		for (int k = 0; k < kLen; ++k) {
			out += static_cast<char>('a' + (k % 26));
		}
		out += '"';
	}
	out += ']';
	return out;
}
// R0 — pretty-printed corpus: ~1 MB flat object, 2-space indent + newlines.
// Exposes skip_ws cost; today's compact corpora hide it.
std::string make_pretty_ws_corpus() {
	std::string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += "{\n";
	constexpr int kMembers = 16000;
	for (int i = 0; i < kMembers; ++i) {
		out += "  \"key_";
		out += std::to_string(i);
		out += "\" : ";
		out += std::to_string(i * 17);
		if (i + 1 < kMembers) {
			out += ',';
		}
		out += '\n';
	}
	out += "}\n";
	return out;
}
// R0 — escape-heavy corpus: a single 256 KiB std::string with backslash escapes
// at high density. Stresses the parse-side slow path (parse_str_decode_tail)
// and the dump-side escape scan.
std::string make_escape_heavy_corpus() {
	std::string out;
	constexpr std::size_t kTarget = 256UZ * 1024UZ;
	out.reserve(kTarget + 16);
	out += '"';
	while (out.size() + 8 < kTarget) {
		out += R"(\n\t\")"; // 6 source bytes → 3 JSON escapes per cycle
	}
	out += '"';
	return out;
}
std::string make_plain_string_payload() {
	std::string out;
	out.reserve(1024UZ * 1024UZ);
	for (std::size_t i = 0; i < 1024UZ * 1024UZ; ++i) {
		out += static_cast<char>('a' + (i % 26UZ));
	}
	return out;
}
std::string make_escape_heavy_string_payload() {
	std::string out;
	constexpr std::size_t kTarget = 256UZ * 1024UZ;
	out.reserve(kTarget);
	while (out.size() + 3 < kTarget) {
		out += '\n';
		out += '\t';
		out += '"';
	}
	return out;
}
// R0 — deeply-nested A: 256 levels of [[…]] with a single 0 at center.
// Tests recursion / iterative parse depth handling without tripping the
// 512-frame default max_depth.
std::string make_deep_nest_corpus() {
	std::string out;
	constexpr int kDepth = 256;
	out.reserve(kDepth * 2 + 4);
	out.append(kDepth, '[');
	out += '0';
	out.append(kDepth, ']');
	return out;
}
// R0 — mixed-number corpus: ~1 MB A of integers, scientific,
// long fractions, signed values. Stresses number-lexeme parse paths.
std::string make_mixed_numbers_corpus() {
	std::string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += '[';
	bool first = true;
	int i = 0;
	constexpr std::size_t kTarget = 1024UZ * 1024UZ - 16;
	while (out.size() < kTarget) {
		if (!first) {
			out += ',';
		}
		first = false;
		switch (i % 4) {
		case 0 : out += std::to_string(i); break;
		case 1 : out += std::format("{}.{}e{}", i, i * 3, (i % 7) - 3); break;
		case 2 : out += std::format("0.{}", i); break;
		case 3 : out += std::format("-{}.{}", i, i * 9); break;
		default: break;
		}
		++i;
	}
	out += ']';
	return out;
}
// ---------------------------------------------------------------------------
// Benchmark drivers
// ---------------------------------------------------------------------------

} // namespace

struct BenchSmall {
	std::int64_t id{};
	bool active{};
};
struct BenchMedium {
	std::int64_t id{};
	std::int64_t count{};
	double score{};
	bool active{};
	std::string name{};
	std::string tag{};
	std::optional<std::int64_t> limit{};
	std::vector<std::int64_t> values{};
};
struct BenchInner {
	std::int64_t x{};
	std::int64_t y{};
};
struct BenchNested {
	std::string id{};
	BenchInner origin{};
	std::vector<BenchSmall> items{};
};

struct BenchWide8 {
	std::int64_t f0{};
	std::int64_t f1{};
	std::int64_t f2{};
	std::int64_t f3{};
	std::int64_t f4{};
	std::int64_t f5{};
	std::int64_t f6{};
	std::int64_t f7{};
};
struct BenchWide16 {
	std::int64_t f0{};
	std::int64_t f1{};
	std::int64_t f2{};
	std::int64_t f3{};
	std::int64_t f4{};
	std::int64_t f5{};
	std::int64_t f6{};
	std::int64_t f7{};
	std::int64_t f8{};
	std::int64_t f9{};
	std::int64_t f10{};
	std::int64_t f11{};
	std::int64_t f12{};
	std::int64_t f13{};
	std::int64_t f14{};
	std::int64_t f15{};
};
struct BenchWide32 {
	std::int64_t f0{};
	std::int64_t f1{};
	std::int64_t f2{};
	std::int64_t f3{};
	std::int64_t f4{};
	std::int64_t f5{};
	std::int64_t f6{};
	std::int64_t f7{};
	std::int64_t f8{};
	std::int64_t f9{};
	std::int64_t f10{};
	std::int64_t f11{};
	std::int64_t f12{};
	std::int64_t f13{};
	std::int64_t f14{};
	std::int64_t f15{};
	std::int64_t f16{};
	std::int64_t f17{};
	std::int64_t f18{};
	std::int64_t f19{};
	std::int64_t f20{};
	std::int64_t f21{};
	std::int64_t f22{};
	std::int64_t f23{};
	std::int64_t f24{};
	std::int64_t f25{};
	std::int64_t f26{};
	std::int64_t f27{};
	std::int64_t f28{};
	std::int64_t f29{};
	std::int64_t f30{};
	std::int64_t f31{};
};
struct BenchWide64 {
	std::int64_t f0{};
	std::int64_t f1{};
	std::int64_t f2{};
	std::int64_t f3{};
	std::int64_t f4{};
	std::int64_t f5{};
	std::int64_t f6{};
	std::int64_t f7{};
	std::int64_t f8{};
	std::int64_t f9{};
	std::int64_t f10{};
	std::int64_t f11{};
	std::int64_t f12{};
	std::int64_t f13{};
	std::int64_t f14{};
	std::int64_t f15{};
	std::int64_t f16{};
	std::int64_t f17{};
	std::int64_t f18{};
	std::int64_t f19{};
	std::int64_t f20{};
	std::int64_t f21{};
	std::int64_t f22{};
	std::int64_t f23{};
	std::int64_t f24{};
	std::int64_t f25{};
	std::int64_t f26{};
	std::int64_t f27{};
	std::int64_t f28{};
	std::int64_t f29{};
	std::int64_t f30{};
	std::int64_t f31{};
	std::int64_t f32{};
	std::int64_t f33{};
	std::int64_t f34{};
	std::int64_t f35{};
	std::int64_t f36{};
	std::int64_t f37{};
	std::int64_t f38{};
	std::int64_t f39{};
	std::int64_t f40{};
	std::int64_t f41{};
	std::int64_t f42{};
	std::int64_t f43{};
	std::int64_t f44{};
	std::int64_t f45{};
	std::int64_t f46{};
	std::int64_t f47{};
	std::int64_t f48{};
	std::int64_t f49{};
	std::int64_t f50{};
	std::int64_t f51{};
	std::int64_t f52{};
	std::int64_t f53{};
	std::int64_t f54{};
	std::int64_t f55{};
	std::int64_t f56{};
	std::int64_t f57{};
	std::int64_t f58{};
	std::int64_t f59{};
	std::int64_t f60{};
	std::int64_t f61{};
	std::int64_t f62{};
	std::int64_t f63{};
};
struct BenchWide96 {
	std::int64_t f0{};
	std::int64_t f1{};
	std::int64_t f2{};
	std::int64_t f3{};
	std::int64_t f4{};
	std::int64_t f5{};
	std::int64_t f6{};
	std::int64_t f7{};
	std::int64_t f8{};
	std::int64_t f9{};
	std::int64_t f10{};
	std::int64_t f11{};
	std::int64_t f12{};
	std::int64_t f13{};
	std::int64_t f14{};
	std::int64_t f15{};
	std::int64_t f16{};
	std::int64_t f17{};
	std::int64_t f18{};
	std::int64_t f19{};
	std::int64_t f20{};
	std::int64_t f21{};
	std::int64_t f22{};
	std::int64_t f23{};
	std::int64_t f24{};
	std::int64_t f25{};
	std::int64_t f26{};
	std::int64_t f27{};
	std::int64_t f28{};
	std::int64_t f29{};
	std::int64_t f30{};
	std::int64_t f31{};
	std::int64_t f32{};
	std::int64_t f33{};
	std::int64_t f34{};
	std::int64_t f35{};
	std::int64_t f36{};
	std::int64_t f37{};
	std::int64_t f38{};
	std::int64_t f39{};
	std::int64_t f40{};
	std::int64_t f41{};
	std::int64_t f42{};
	std::int64_t f43{};
	std::int64_t f44{};
	std::int64_t f45{};
	std::int64_t f46{};
	std::int64_t f47{};
	std::int64_t f48{};
	std::int64_t f49{};
	std::int64_t f50{};
	std::int64_t f51{};
	std::int64_t f52{};
	std::int64_t f53{};
	std::int64_t f54{};
	std::int64_t f55{};
	std::int64_t f56{};
	std::int64_t f57{};
	std::int64_t f58{};
	std::int64_t f59{};
	std::int64_t f60{};
	std::int64_t f61{};
	std::int64_t f62{};
	std::int64_t f63{};
	std::int64_t f64{};
	std::int64_t f65{};
	std::int64_t f66{};
	std::int64_t f67{};
	std::int64_t f68{};
	std::int64_t f69{};
	std::int64_t f70{};
	std::int64_t f71{};
	std::int64_t f72{};
	std::int64_t f73{};
	std::int64_t f74{};
	std::int64_t f75{};
	std::int64_t f76{};
	std::int64_t f77{};
	std::int64_t f78{};
	std::int64_t f79{};
	std::int64_t f80{};
	std::int64_t f81{};
	std::int64_t f82{};
	std::int64_t f83{};
	std::int64_t f84{};
	std::int64_t f85{};
	std::int64_t f86{};
	std::int64_t f87{};
	std::int64_t f88{};
	std::int64_t f89{};
	std::int64_t f90{};
	std::int64_t f91{};
	std::int64_t f92{};
	std::int64_t f93{};
	std::int64_t f94{};
	std::int64_t f95{};
};
template<>
struct conflux::json::JsonMembers<BenchSmall> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &BenchSmall::id),
			json_member("active", &BenchSmall::active),
		};
	}
	static constexpr std::string_view type_name() { return "BenchSmall"; }
};
template<>
struct conflux::json::JsonMembers<BenchMedium> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &BenchMedium::id),
			json_member("count", &BenchMedium::count),
			json_member("score", &BenchMedium::score),
			json_member("active", &BenchMedium::active),
			json_member("name", &BenchMedium::name),
			json_member("tag", &BenchMedium::tag),
			json_member("limit", &BenchMedium::limit),
			json_member("values", &BenchMedium::values),
		};
	}
	static constexpr std::string_view type_name() { return "BenchMedium"; }
};
template<>
struct conflux::json::JsonMembers<BenchInner> {
	static constexpr auto members() {
		return std::tuple{
			json_member("x", &BenchInner::x),
			json_member("y", &BenchInner::y),
		};
	}
	static constexpr std::string_view type_name() { return "BenchInner"; }
};
template<>
struct conflux::json::JsonMembers<BenchNested> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &BenchNested::id),
			json_member("origin", &BenchNested::origin),
			json_member("items", &BenchNested::items),
		};
	}
	static constexpr std::string_view type_name() { return "BenchNested"; }
};

template<>
struct conflux::json::JsonMembers<BenchWide8> {
	static constexpr auto members() {
		return std::tuple{
			json_member("f0", &BenchWide8::f0),
			json_member("f1", &BenchWide8::f1),
			json_member("f2", &BenchWide8::f2),
			json_member("f3", &BenchWide8::f3),
			json_member("f4", &BenchWide8::f4),
			json_member("f5", &BenchWide8::f5),
			json_member("f6", &BenchWide8::f6),
			json_member("f7", &BenchWide8::f7)};
	}
	static constexpr std::string_view type_name() { return "BenchWide8"; }
};
template<>
struct conflux::json::JsonMembers<BenchWide16> {
	static constexpr auto members() {
		return std::tuple{
			json_member("f0", &BenchWide16::f0),
			json_member("f1", &BenchWide16::f1),
			json_member("f2", &BenchWide16::f2),
			json_member("f3", &BenchWide16::f3),
			json_member("f4", &BenchWide16::f4),
			json_member("f5", &BenchWide16::f5),
			json_member("f6", &BenchWide16::f6),
			json_member("f7", &BenchWide16::f7),
			json_member("f8", &BenchWide16::f8),
			json_member("f9", &BenchWide16::f9),
			json_member("f10", &BenchWide16::f10),
			json_member("f11", &BenchWide16::f11),
			json_member("f12", &BenchWide16::f12),
			json_member("f13", &BenchWide16::f13),
			json_member("f14", &BenchWide16::f14),
			json_member("f15", &BenchWide16::f15)};
	}
	static constexpr std::string_view type_name() { return "BenchWide16"; }
};
template<>
struct conflux::json::JsonMembers<BenchWide32> {
	static constexpr auto members() {
		return std::tuple{json_member("f0", &BenchWide32::f0),   json_member("f1", &BenchWide32::f1),
						  json_member("f2", &BenchWide32::f2),   json_member("f3", &BenchWide32::f3),
						  json_member("f4", &BenchWide32::f4),   json_member("f5", &BenchWide32::f5),
						  json_member("f6", &BenchWide32::f6),   json_member("f7", &BenchWide32::f7),
						  json_member("f8", &BenchWide32::f8),   json_member("f9", &BenchWide32::f9),
						  json_member("f10", &BenchWide32::f10), json_member("f11", &BenchWide32::f11),
						  json_member("f12", &BenchWide32::f12), json_member("f13", &BenchWide32::f13),
						  json_member("f14", &BenchWide32::f14), json_member("f15", &BenchWide32::f15),
						  json_member("f16", &BenchWide32::f16), json_member("f17", &BenchWide32::f17),
						  json_member("f18", &BenchWide32::f18), json_member("f19", &BenchWide32::f19),
						  json_member("f20", &BenchWide32::f20), json_member("f21", &BenchWide32::f21),
						  json_member("f22", &BenchWide32::f22), json_member("f23", &BenchWide32::f23),
						  json_member("f24", &BenchWide32::f24), json_member("f25", &BenchWide32::f25),
						  json_member("f26", &BenchWide32::f26), json_member("f27", &BenchWide32::f27),
						  json_member("f28", &BenchWide32::f28), json_member("f29", &BenchWide32::f29),
						  json_member("f30", &BenchWide32::f30), json_member("f31", &BenchWide32::f31)};
	}
	static constexpr std::string_view type_name() { return "BenchWide32"; }
};
template<>
struct conflux::json::JsonMembers<BenchWide64> {
	static constexpr auto members() {
		return std::tuple{json_member("f0", &BenchWide64::f0),   json_member("f1", &BenchWide64::f1),
						  json_member("f2", &BenchWide64::f2),   json_member("f3", &BenchWide64::f3),
						  json_member("f4", &BenchWide64::f4),   json_member("f5", &BenchWide64::f5),
						  json_member("f6", &BenchWide64::f6),   json_member("f7", &BenchWide64::f7),
						  json_member("f8", &BenchWide64::f8),   json_member("f9", &BenchWide64::f9),
						  json_member("f10", &BenchWide64::f10), json_member("f11", &BenchWide64::f11),
						  json_member("f12", &BenchWide64::f12), json_member("f13", &BenchWide64::f13),
						  json_member("f14", &BenchWide64::f14), json_member("f15", &BenchWide64::f15),
						  json_member("f16", &BenchWide64::f16), json_member("f17", &BenchWide64::f17),
						  json_member("f18", &BenchWide64::f18), json_member("f19", &BenchWide64::f19),
						  json_member("f20", &BenchWide64::f20), json_member("f21", &BenchWide64::f21),
						  json_member("f22", &BenchWide64::f22), json_member("f23", &BenchWide64::f23),
						  json_member("f24", &BenchWide64::f24), json_member("f25", &BenchWide64::f25),
						  json_member("f26", &BenchWide64::f26), json_member("f27", &BenchWide64::f27),
						  json_member("f28", &BenchWide64::f28), json_member("f29", &BenchWide64::f29),
						  json_member("f30", &BenchWide64::f30), json_member("f31", &BenchWide64::f31),
						  json_member("f32", &BenchWide64::f32), json_member("f33", &BenchWide64::f33),
						  json_member("f34", &BenchWide64::f34), json_member("f35", &BenchWide64::f35),
						  json_member("f36", &BenchWide64::f36), json_member("f37", &BenchWide64::f37),
						  json_member("f38", &BenchWide64::f38), json_member("f39", &BenchWide64::f39),
						  json_member("f40", &BenchWide64::f40), json_member("f41", &BenchWide64::f41),
						  json_member("f42", &BenchWide64::f42), json_member("f43", &BenchWide64::f43),
						  json_member("f44", &BenchWide64::f44), json_member("f45", &BenchWide64::f45),
						  json_member("f46", &BenchWide64::f46), json_member("f47", &BenchWide64::f47),
						  json_member("f48", &BenchWide64::f48), json_member("f49", &BenchWide64::f49),
						  json_member("f50", &BenchWide64::f50), json_member("f51", &BenchWide64::f51),
						  json_member("f52", &BenchWide64::f52), json_member("f53", &BenchWide64::f53),
						  json_member("f54", &BenchWide64::f54), json_member("f55", &BenchWide64::f55),
						  json_member("f56", &BenchWide64::f56), json_member("f57", &BenchWide64::f57),
						  json_member("f58", &BenchWide64::f58), json_member("f59", &BenchWide64::f59),
						  json_member("f60", &BenchWide64::f60), json_member("f61", &BenchWide64::f61),
						  json_member("f62", &BenchWide64::f62), json_member("f63", &BenchWide64::f63)};
	}
	static constexpr std::string_view type_name() { return "BenchWide64"; }
};
template<>
struct conflux::json::JsonMembers<BenchWide96> {
	static constexpr auto members() {
		return std::tuple{json_member("f0", &BenchWide96::f0),   json_member("f1", &BenchWide96::f1),
						  json_member("f2", &BenchWide96::f2),   json_member("f3", &BenchWide96::f3),
						  json_member("f4", &BenchWide96::f4),   json_member("f5", &BenchWide96::f5),
						  json_member("f6", &BenchWide96::f6),   json_member("f7", &BenchWide96::f7),
						  json_member("f8", &BenchWide96::f8),   json_member("f9", &BenchWide96::f9),
						  json_member("f10", &BenchWide96::f10), json_member("f11", &BenchWide96::f11),
						  json_member("f12", &BenchWide96::f12), json_member("f13", &BenchWide96::f13),
						  json_member("f14", &BenchWide96::f14), json_member("f15", &BenchWide96::f15),
						  json_member("f16", &BenchWide96::f16), json_member("f17", &BenchWide96::f17),
						  json_member("f18", &BenchWide96::f18), json_member("f19", &BenchWide96::f19),
						  json_member("f20", &BenchWide96::f20), json_member("f21", &BenchWide96::f21),
						  json_member("f22", &BenchWide96::f22), json_member("f23", &BenchWide96::f23),
						  json_member("f24", &BenchWide96::f24), json_member("f25", &BenchWide96::f25),
						  json_member("f26", &BenchWide96::f26), json_member("f27", &BenchWide96::f27),
						  json_member("f28", &BenchWide96::f28), json_member("f29", &BenchWide96::f29),
						  json_member("f30", &BenchWide96::f30), json_member("f31", &BenchWide96::f31),
						  json_member("f32", &BenchWide96::f32), json_member("f33", &BenchWide96::f33),
						  json_member("f34", &BenchWide96::f34), json_member("f35", &BenchWide96::f35),
						  json_member("f36", &BenchWide96::f36), json_member("f37", &BenchWide96::f37),
						  json_member("f38", &BenchWide96::f38), json_member("f39", &BenchWide96::f39),
						  json_member("f40", &BenchWide96::f40), json_member("f41", &BenchWide96::f41),
						  json_member("f42", &BenchWide96::f42), json_member("f43", &BenchWide96::f43),
						  json_member("f44", &BenchWide96::f44), json_member("f45", &BenchWide96::f45),
						  json_member("f46", &BenchWide96::f46), json_member("f47", &BenchWide96::f47),
						  json_member("f48", &BenchWide96::f48), json_member("f49", &BenchWide96::f49),
						  json_member("f50", &BenchWide96::f50), json_member("f51", &BenchWide96::f51),
						  json_member("f52", &BenchWide96::f52), json_member("f53", &BenchWide96::f53),
						  json_member("f54", &BenchWide96::f54), json_member("f55", &BenchWide96::f55),
						  json_member("f56", &BenchWide96::f56), json_member("f57", &BenchWide96::f57),
						  json_member("f58", &BenchWide96::f58), json_member("f59", &BenchWide96::f59),
						  json_member("f60", &BenchWide96::f60), json_member("f61", &BenchWide96::f61),
						  json_member("f62", &BenchWide96::f62), json_member("f63", &BenchWide96::f63),
						  json_member("f64", &BenchWide96::f64), json_member("f65", &BenchWide96::f65),
						  json_member("f66", &BenchWide96::f66), json_member("f67", &BenchWide96::f67),
						  json_member("f68", &BenchWide96::f68), json_member("f69", &BenchWide96::f69),
						  json_member("f70", &BenchWide96::f70), json_member("f71", &BenchWide96::f71),
						  json_member("f72", &BenchWide96::f72), json_member("f73", &BenchWide96::f73),
						  json_member("f74", &BenchWide96::f74), json_member("f75", &BenchWide96::f75),
						  json_member("f76", &BenchWide96::f76), json_member("f77", &BenchWide96::f77),
						  json_member("f78", &BenchWide96::f78), json_member("f79", &BenchWide96::f79),
						  json_member("f80", &BenchWide96::f80), json_member("f81", &BenchWide96::f81),
						  json_member("f82", &BenchWide96::f82), json_member("f83", &BenchWide96::f83),
						  json_member("f84", &BenchWide96::f84), json_member("f85", &BenchWide96::f85),
						  json_member("f86", &BenchWide96::f86), json_member("f87", &BenchWide96::f87),
						  json_member("f88", &BenchWide96::f88), json_member("f89", &BenchWide96::f89),
						  json_member("f90", &BenchWide96::f90), json_member("f91", &BenchWide96::f91),
						  json_member("f92", &BenchWide96::f92), json_member("f93", &BenchWide96::f93),
						  json_member("f94", &BenchWide96::f94), json_member("f95", &BenchWide96::f95)};
	}
	static constexpr std::string_view type_name() { return "BenchWide96"; }
};

namespace {

[[nodiscard]] std::string make_medium_json(
	bool out_of_order = false,
	bool escaped = false) {
	if (out_of_order) {
		return R"({"values":[1,2,3,4,5,6,7,8],"limit":64,"tag":"direct","name":"bench","active":true,"score":12.5,"count":42,"id":7})";
	}
	if (escaped) {
		return R"({"id":7,"count":42,"score":12.5,"active":true,"name":"bench\nname","tag":"direct\u002ftag","limit":64,"values":[1,2,3,4,5,6,7,8]})";
	}
	return R"({"id":7,"count":42,"score":12.5,"active":true,"name":"bench","tag":"direct","limit":64,"values":[1,2,3,4,5,6,7,8]})";
}
[[nodiscard]] std::string make_nested_json() {
	return R"({"id":"root","origin":{"x":3,"y":4},"items":[{"id":1,"active":true},{"id":2,"active":false},{"id":3,"active":true}]})";
}
[[nodiscard]] std::string make_array_objects_json() {
	std::string out;
	out.reserve(4096);
	out += '[';
	for (int i = 0; i < 64; ++i) {
		if (i != 0) {
			out += ',';
		}
		out += std::format(R"({{"id":{},"active":{}}})", i, (i % 2 == 0) ? "true" : "false");
	}
	out += ']';
	return out;
}

[[nodiscard]] std::string make_wide_object_json(
	std::size_t known_fields,
	std::size_t unknown_fields = 0) {
	std::string out;
	out.reserve((known_fields + unknown_fields) * 18 + 8);
	out += '{';
	bool first = true;
	auto append_sep = [&] {
		if (!first) {
			out += ',';
		}
		first = false;
	};
	for (std::size_t i = 0; i < known_fields; ++i) {
		append_sep();
		out += std::format(R"("f{}":{})", i, i);
		if (i < unknown_fields) {
			append_sep();
			out += std::format(R"("extra_{}":{})", i, i);
		}
	}
	for (std::size_t i = known_fields; i < unknown_fields; ++i) {
		append_sep();
		out += std::format(R"("extra_{}":{})", i, i);
	}
	out += '}';
	return out;
}
template<class T>
void require_decode(
	std::expected<T, JsonError> value) {
	if (!value) {
		throw std::runtime_error{value.error().message};
	}
}
void require_dump(
	std::expected<std::string, JsonError> value) {
	if (!value) {
		throw std::runtime_error{value.error().message};
	}
}
template<class T>
void require_dom_write(
	T const &value) {
	auto b = value_builder();
	if (auto ok = b.set<T>(value); !ok) {
		throw std::runtime_error{ok.error().message};
	}
	auto doc = std::move(b).finish();
	if (!doc) {
		throw std::runtime_error{doc.error().message};
	}
	auto dumped = doc->dump();
	if (!dumped) {
		throw std::runtime_error{dumped.error().message};
	}
}
void bench_direct_struct_matrix() {
	std::string const small = R"({"id":7,"active":true})";
	std::string const medium = make_medium_json();
	std::string const nested = make_nested_json();
	std::string const array_objects = make_array_objects_json();
	std::string const out_of_order = make_medium_json(true);
	std::string const escaped = make_medium_json(false, true);
	BenchMedium const medium_value{
		.id = 7,
		.count = 42,
		.score = 12.5,
		.active = true,
		.name = "bench",
		.tag = "direct",
		.limit = 64,
		.values = {1, 2, 3, 4, 5, 6, 7, 8},
	};
	BenchNested const nested_value{
		.id = "root",
		.origin = {.x = 3, .y = 4},
		.items = {{.id = 1, .active = true}, {.id = 2, .active = false}, {.id = 3, .active = true}},
	};

	run_alloc_row("decode/manual/dom/small", [&] {
		return measure_alloc(
			[&] {
				auto doc = parse(small);
				if (!doc) {
					throw std::runtime_error{doc.error().message};
				}
				require_decode(decode<BenchSmall>(doc->root()));
			},
			100,
			500,
			1,
			small.size());
	});
	run_alloc_row("decode/manual/reader/direct/small", [&] {
		return measure_alloc([&] { require_decode(decode_borrowed<BenchSmall>(small)); }, 100, 500, 1, small.size());
	});
	run_alloc_row("decode/manual/reader/direct/medium", [&] {
		return measure_alloc([&] { require_decode(decode_borrowed<BenchMedium>(medium)); }, 100, 500, 1, medium.size());
	});
	run_alloc_row("decode/manual/reader/direct/nested", [&] {
		return measure_alloc([&] { require_decode(decode_borrowed<BenchNested>(nested)); }, 100, 500, 1, nested.size());
	});
	run_alloc_row("decode/manual/reader/direct/array_objects", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<std::vector<BenchSmall>>(array_objects)); },
			100,
			500,
			1,
			array_objects.size());
	});
	run_alloc_row("decode/manual/reader/direct/out_of_order", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<BenchMedium>(out_of_order)); },
			100,
			500,
			1,
			out_of_order.size());
	});
	run_alloc_row("decode/manual/reader/direct/escaped_strings", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<BenchMedium>(escaped)); },
			100,
			500,
			1,
			escaped.size());
	});
	run_alloc_row("write/manual/dom", [&] {
		return measure_alloc([&] { require_dom_write(medium_value); }, 100, 500, 1, medium.size());
	});
	run_alloc_row("write/manual/direct", [&] {
		return measure_alloc([&] { require_dump(dump_direct(medium_value)); }, 100, 500, 1, medium.size());
	});
	run_alloc_row("write/manual/direct/nested", [&] {
		return measure_alloc([&] { require_dump(dump_direct(nested_value)); }, 100, 500, 1, nested.size());
	});
}

void bench_direct_wide_object_matrix() {
	std::string const wide8 = make_wide_object_json(8);
	std::string const wide16 = make_wide_object_json(16);
	std::string const wide32 = make_wide_object_json(32);
	std::string const wide64 = make_wide_object_json(64);
	std::string const wide96 = make_wide_object_json(96);
	std::string const wide32_unknown = make_wide_object_json(32, 32);
	std::string const wide64_unknown = make_wide_object_json(64, 64);
	std::string const wide96_unknown = make_wide_object_json(96, 96);
	JsonDecodeOptions ignore_unknown;
	ignore_unknown.unknown_members = UnknownMemberPolicy::ignore;

	run_alloc_row("decode/manual/reader/direct/wide08/all_known", [&] {
		return measure_alloc([&] { require_decode(decode_borrowed<BenchWide8>(wide8)); }, 100, 500, 1, wide8.size());
	});
	run_alloc_row("decode/manual/reader/direct/wide16/all_known", [&] {
		return measure_alloc([&] { require_decode(decode_borrowed<BenchWide16>(wide16)); }, 100, 500, 1, wide16.size());
	});
	run_alloc_row("decode/manual/reader/direct/wide32/all_known", [&] {
		return measure_alloc([&] { require_decode(decode_borrowed<BenchWide32>(wide32)); }, 100, 500, 1, wide32.size());
	});
	run_alloc_row("decode/manual/reader/direct/wide64/all_known", [&] {
		return measure_alloc([&] { require_decode(decode_borrowed<BenchWide64>(wide64)); }, 100, 500, 1, wide64.size());
	});
	run_alloc_row("decode/manual/reader/direct/wide96/all_known", [&] {
		return measure_alloc([&] { require_decode(decode_borrowed<BenchWide96>(wide96)); }, 100, 500, 1, wide96.size());
	});
	run_alloc_row("decode/manual/reader/direct/wide32/unknown_ignore", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<BenchWide32>(wide32_unknown, {}, ignore_unknown)); },
			100,
			500,
			1,
			wide32_unknown.size());
	});
	run_alloc_row("decode/manual/reader/direct/wide64/unknown_ignore", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<BenchWide64>(wide64_unknown, {}, ignore_unknown)); },
			100,
			500,
			1,
			wide64_unknown.size());
	});
	run_alloc_row("decode/manual/reader/direct/wide96/unknown_ignore", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<BenchWide96>(wide96_unknown, {}, ignore_unknown)); },
			100,
			500,
			1,
			wide96_unknown.size());
	});
}

void bench_parse_small(
	std::string const &corpus) {
	run_row("parse/small (~4KB config)", [&] {
		return measure([&] { (void)parse(corpus); }, 100, 500, 1, corpus.size());
	});
}
void bench_parse_large(
	std::string const &corpus) {
	run_row("parse/large (~1MB nested)", [&] {
		return measure([&] { (void)parse(corpus); }, 4, 20, 1, corpus.size());
	});
}
void bench_decode(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	// Measure: parse + extract name field as std::string_view from each object
	run_row("decode/struct-like (sv fields)", [&] {
		return measure(
			[&] {
				auto res = parse(corpus);
				if (!res) {
					return;
				}
				auto arr = res->root().as_array();
				if (!arr) {
					return;
				}
				for (NodeRef const elem: arr->elements()) {
					auto obj = elem.as_object();
					if (!obj) {
						continue;
					}
					auto name = obj->find_member("name");
					if (name) {
						(void)name->as_string();
					}
				}
			},
			20,
			100,
			1,
			corpus.size());
	});
}
void bench_find_member(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root()); // pre-build hash index
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	// batch=1000: amortise clock overhead for sub-microsecond lookup
	run_row("find_member/1024-member object (per lookup)", [&] {
		auto s = measure(
			[&] {
				(void)obj->find_member("member_0");
				(void)obj->find_member("member_511");
				(void)obj->find_member("member_1023");
			},
			100,
			500,
			1000);
		s.ns_per_iter /= 3.0;
		return s;
	});
}
void bench_array_traversal(
	std::string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto arr = doc->root().as_array();
	if (!arr) {
		return;
	}
	run_row("A/traverse 10k numbers", [&] {
		return measure(
			[&] {
				std::int64_t sum = 0;
				for (NodeRef const elem: arr->elements()) {
					auto n = elem.as_number();
					if (n) {
						auto v = n->to_i64();
						if (v) {
							sum += *v;
						}
					}
				}
				(void)sum;
			},
			100,
			500);
	});
}
void bench_builder() {
	run_row("builder/64-member object", [&] {
		return measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (int i = 0; i < 64; ++i) {
					(void)obj->insert_string(std::format("key_{}", i), std::format("value_{}", i));
				}
				std::move(*obj).commit();
				(void)std::move(b).finish();
			},
			100,
			500);
	});
}
void bench_dump_plain(
	std::string const &corpus) {
	if (!should_run("dump/plain (no sort / no ascii_only)")) {
		return;
	}
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto json_str = doc->dump();
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(); }, 100, 500, 1, json_str->size());
	print_row("dump/plain (no sort / no ascii_only)", s);
}
void bench_dump_sorted(
	std::string const &corpus) {
	if (!should_run("dump/sort_object_keys")) {
		return;
	}
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	JsonDumpOptions opts;
	opts.sort_object_keys = true;
	auto json_str = doc->dump(opts);
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(opts); }, 40, 200, 1, json_str->size());
	print_row("dump/sort_object_keys", s);
}
void bench_accumulate_chunked(
	std::string_view name,
	std::string const &corpus,
	std::size_t chunk_size) {
	if (!should_run(name)) {
		return;
	}
	auto s = measure(
		[&] {
			JsonAccumulator acc;
			auto const *ptr = corpus.data();
			std::size_t remaining = corpus.size();
			while (remaining > 0) {
				std::size_t const n = std::min(chunk_size, remaining);
				auto feed = acc.feed(std::span<std::byte const>{reinterpret_cast<std::byte const *>(ptr), n});
				if (!feed) {
					throw std::runtime_error{"json accumulator feed failed"};
				}
				ptr += n;
				remaining -= n;
			}
			auto doc = acc.finish();
			if (!doc) {
				throw std::runtime_error{"json accumulator finish failed"};
			}
			(void)doc->root();
		},
		20,
		100,
		1,
		corpus.size());
	print_row(name, s);
}
[[nodiscard]] int start_listener(
	std::uint16_t &port_out) {
	int const fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		throw std::runtime_error{"socket"};
	}
	int one = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
		::close(fd);
		throw std::runtime_error{"bind"};
	}
	socklen_t slen = sizeof(addr);
	if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &slen) < 0) {
		::close(fd);
		throw std::runtime_error{"getsockname"};
	}
	port_out = ::ntohs(addr.sin_port);
	if (::listen(fd, 16) < 0) {
		::close(fd);
		throw std::runtime_error{"listen"};
	}
	return fd;
}
[[nodiscard]] sockaddr_storage loopback_addr(
	std::uint16_t port) noexcept {
	sockaddr_storage ss{};
	auto *sin = reinterpret_cast<sockaddr_in *>(&ss);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
	sin->sin_port = ::htons(port);
	return ss;
}
void send_all(
	int fd,
	std::string_view data) {
	auto const *ptr = data.data();
	std::size_t remaining = data.size();
	while (remaining > 0) {
		ssize_t const n = ::send(fd, ptr, remaining, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw std::runtime_error{"send"};
		}
		ptr += static_cast<std::size_t>(n);
		remaining -= static_cast<std::size_t>(n);
	}
}
void serve_json_corpus(
	int listener_fd,
	std::atomic_flag &stop,
	std::string_view corpus) {
	timeval tv{.tv_sec = 0, .tv_usec = 100000};
	(void)::setsockopt(listener_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	while (!stop.test(std::memory_order_acquire)) {
		int const cfd = ::accept4(listener_fd, nullptr, nullptr, SOCK_CLOEXEC);
		if (cfd < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			break;
		}
		try {
			send_all(cfd, corpus);
			::shutdown(cfd, SHUT_WR);
		} catch (...) {
			::close(cfd);
			break;
		}
		::close(cfd);
	}
	::close(listener_fd);
}
struct TempCorpusFile {
	std::filesystem::path path;
	explicit TempCorpusFile(
		std::string_view corpus) {
		path = std::filesystem::temp_directory_path() / std::format("conflux_json_bench_e2e_{}.json", ::getpid());
		std::ofstream out{path, std::ios::binary | std::ios::trunc};
		if (!out) {
			throw std::runtime_error{std::format("cannot open {}", path.string())};
		}
		out.write(corpus.data(), static_cast<std::streamsize>(corpus.size()));
		if (!out) {
			throw std::runtime_error{std::format("cannot write {}", path.string())};
		}
	}
	~TempCorpusFile() {
		std::error_code ec;
		(void)std::filesystem::remove(path, ec);
	}
};
conflux::work::root::Task<void> decode_file_once(
	conflux::file_io::FileReader &files,
	std::string path) {
	auto handle = co_await files.async_open(AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
	JsonAccumulator acc;
	std::array<std::uint8_t, 8192> buf{};
	std::uint64_t off = 0;
	for (;;) {
		auto got = co_await files.read_into(handle, off, std::as_writable_bytes(std::span{buf}));
		if (got == 0) {
			break;
		}
		off += got;
		auto feed = acc.feed(std::span<std::byte const>{reinterpret_cast<std::byte const *>(buf.data()), got});
		if (!feed) {
			throw std::runtime_error{"json accumulator feed failed"};
		}
	}
	auto doc = acc.finish();
	if (!doc) {
		throw std::runtime_error{"json accumulator finish failed"};
	}
	(void)doc->root();
}
conflux::work::root::Task<void> decode_socket_once(
	conflux::socket_io::SocketTaskRing &ring,
	std::uint16_t port) {
	auto ss = loopback_addr(port);
	auto stream = co_await async_tcp_connect(ring, AF_INET, ss, sizeof(sockaddr_in));
	JsonAccumulator acc;
	std::array<std::uint8_t, 8192> buf{};
	for (;;) {
		auto got = co_await stream.async_recv_borrowed(std::span<std::uint8_t>{buf.data(), buf.size()});
		if (got == 0) {
			break;
		}
		auto feed = acc.feed(std::span<std::byte const>{reinterpret_cast<std::byte const *>(buf.data()), got});
		if (!feed) {
			throw std::runtime_error{"json accumulator feed failed"};
		}
	}
	auto doc = acc.finish();
	if (!doc) {
		throw std::runtime_error{"json accumulator finish failed"};
	}
	(void)doc->root();
}
void bench_e2e_decode(
	std::string_view name,
	std::string const &corpus) {
	std::string const file_name = std::format("{}/file_reader", name);
	std::string const socket_name = std::format("{}/socket_task_ring", name);
	if (!should_run(file_name) && !should_run(socket_name)) {
		return;
	}
	TempCorpusFile const temp{corpus};
	std::string const file_path = temp.path.string();
	::io_uring raw{};
	if (::io_uring_queue_init(64, &raw, 0) < 0) {
		throw std::runtime_error{"io_uring_queue_init"};
	}
	conflux::uring::CompletionTable ct;
	auto const pack_ud = [](std::uint32_t s, std::uint32_t g) noexcept -> std::uint64_t {
		return (static_cast<std::uint64_t>(g) << 32U) | s;
	};
	conflux::file_io::FileReader files{&raw, &ct, pack_ud};
	conflux::socket_io::SocketTaskRing ring{conflux::socket_io::SocketRawRing{&raw}, ct, pack_ud};
	try {
		if (should_run(file_name)) {
			auto file_stats =
				measure([&] { block_on(files, decode_file_once(files, file_path)); }, 4, 20, 1, corpus.size());
			print_row(file_name, file_stats);
		}
		if (should_run(socket_name)) {
			std::uint16_t port = 0;
			int listener_fd = start_listener(port);
			std::atomic_flag stop{};
			std::thread server{[&] { serve_json_corpus(listener_fd, stop, corpus); }};
			try {
				auto socket_stats = measure(
					[&] { sync_wait_socket_task(ring, decode_socket_once(ring, port)); },
					4,
					20,
					1,
					corpus.size());
				print_row(socket_name, socket_stats);
			} catch (...) {
				stop.test_and_set(std::memory_order_release);
				(void)::shutdown(listener_fd, SHUT_RDWR);
				if (server.joinable()) {
					server.join();
				}
				throw;
			}
			stop.test_and_set(std::memory_order_release);
			(void)::shutdown(listener_fd, SHUT_RDWR);
			if (server.joinable()) {
				server.join();
			}
		}
	} catch (...) {
		::io_uring_queue_exit(&raw);
		throw;
	}
	::io_uring_queue_exit(&raw);
}
// Item C — 1024-member object where every key has a \u escape → arena storage.
// Decoded names are identical to make_lookup_corpus() ("member_N"), so the
// same lookup keys can be used for apples-to-apples comparison.
std::string make_lookup_escaped_corpus() {
	// Keys: "member_N" (JSON) → decoded "member_N".
	// All MemberEntry flags = 0 (arena); kStorageInputView never set.
	std::string out;
	out.reserve(65536);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format("\"\\u006Dember_{}\":{}", i, i);
	}
	out += '}';
	return out;
}
// Item C — 1024-member object with alternating plain/escaped keys.
// Even indices: plain ("member_N", kStorageInputView).
// Odd indices:  "member_N" decoded to "member_N" (arena storage).
// Half-half pattern is worst-case for branch prediction in member_name() dispatch.
std::string make_lookup_mixed_corpus() {
	std::string out;
	out.reserve(65536);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		if (i % 2 == 0) {
			out += std::format("\"member_{}\":{}", i, i);
		} else {
			out += std::format("\"\\u006Dember_{}\":{}", i, i);
		}
	}
	out += '}';
	return out;
}
// FI-1 — small object (below kHashThreshold=32): find_member always does linear
// scan. Proxy for per-lookup cost after the sentinel caches a build failure.
std::string make_below_threshold_corpus() {
	std::string out = "{";
	for (int i = 0; i < 7; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(R"("field_{}":{})", i, i);
	}
	out += '}';
	return out;
}
// 5.5-B gate: 31-member object — always linear (just below kHashThreshold=32).
// Isolates cache-line packing benefit of 16-std::byte vs 24-std::byte MemberEntry.
std::string make_linear31_corpus() {
	std::string out = "{";
	for (int i = 0; i < 31; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += std::format(R"("member_{}":{},)", i, i);
		out.pop_back(); // remove trailing comma left by format string
	}
	out += '}';
	return out;
}
void bench_find_member_linear31(
	std::string const &corpus) {
	if (!should_run("find_member/31-member linear (per lookup)")) {
		return;
	}
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_15");
			(void)obj->find_member("member_30");
		},
		100,
		500,
		1000);
	s.ns_per_iter /= 3.0;
	print_row("find_member/31-member linear (per lookup)", s);
}
// Item C — probe throughput on arena-storage names (baseline: bench_find_member
// uses kStorageInputView names). Delta isolates member_name() dispatch overhead.
void bench_find_member_escaped(
	std::string const &corpus) {
	if (!should_run("find_member/1024-member escaped names (per lookup)")) {
		return;
	}
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root());
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_511");
			(void)obj->find_member("member_1023");
		},
		100,
		500,
		1000);
	s.ns_per_iter /= 3.0;
	print_row("find_member/1024-member escaped names (per lookup)", s);
}
// Item C — worst-case dispatch: alternating kStorageInputView/arena per probe.
void bench_find_member_mixed(
	std::string const &corpus) {
	if (!should_run("find_member/1024-member mixed names (per lookup)")) {
		return;
	}
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root());
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0"); // plain (even)
			(void)obj->find_member("member_511"); // escaped (odd)
			(void)obj->find_member("member_1023"); // escaped (odd)
		},
		100,
		500,
		1000);
	s.ns_per_iter /= 3.0;
	print_row("find_member/1024-member mixed names (per lookup)", s);
}
// Item E — builder name-copy cost: same value ("v"), varying key length.
// If per-insert ns scales with key length, arena copy is the hot path.
// If flat, overhead is in tree structure — name-copy optimisation is not justified.
void bench_builder_name_length() {
	constexpr int kMembers = 256;
	auto gen_keys = [](std::size_t n, std::size_t total_len) {
		std::vector<std::string> keys;
		keys.reserve(n);
		for (std::size_t i = 0; i < n; ++i) {
			std::string const suffix = std::to_string(i);
			std::size_t const pad = total_len > suffix.size() ? total_len - suffix.size() : 0;
			std::string k(pad, 'k');
			k += suffix;
			keys.push_back(std::move(k));
		}
		return keys;
	};
	std::vector<std::string> const k5 = gen_keys(static_cast<std::size_t>(kMembers), 5);
	std::vector<std::string> const k32 = gen_keys(static_cast<std::size_t>(kMembers), 32);
	std::vector<std::string> const k128 = gen_keys(static_cast<std::size_t>(kMembers), 128);

	auto run = [&](std::vector<std::string> const &keys, std::string_view label) {
		if (!should_run(label)) {
			return;
		}
		auto s = measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (int i = 0; i < kMembers; ++i) {
					(void)obj->insert_string(keys[static_cast<std::size_t>(i)], "v");
				}
				std::move(*obj).commit();
				(void)std::move(b).finish();
			},
			100,
			500);
		s.ns_per_iter /= static_cast<double>(kMembers);
		print_row(label, s);
	};

	run(k5, "builder/insert_string   5-char keys (per insert)");
	run(k32, "builder/insert_string  32-char keys (per insert)");
	run(k128, "builder/insert_string 128-char keys (per insert)");

	auto run_view = [&](std::vector<std::string> const &keys, std::string_view label) {
		if (!should_run(label)) {
			return;
		}
		auto s = measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (std::size_t i = 0; i < static_cast<std::size_t>(kMembers); ++i) {
					(void)obj->insert_string_borrowed_name(keys[i], "v");
				}
				std::move(*obj).commit();
				(void)std::move(b).finish();
			},
			100,
			500);
		s.ns_per_iter /= static_cast<double>(kMembers);
		print_row(label, s);
	};

	run_view(k5, "builder/insert_string_borrowed_name   5-char keys (per insert)");
	run_view(k32, "builder/insert_string_borrowed_name  32-char keys (per insert)");
	run_view(k128, "builder/insert_string_borrowed_name 128-char keys (per insert)");
}
// R0 — generic parse/dump drivers used for the new corpora.
void bench_parse_named(
	std::string_view name,
	std::string const &corpus,
	std::size_t warmup = 10,
	std::size_t iters = 50) {
	if (!should_run(name)) {
		return;
	}
	auto s = measure([&] { (void)parse(corpus); }, warmup, iters, 1, corpus.size());
	print_row(name, s);
}
void bench_dump_named(
	std::string_view name,
	std::string const &corpus,
	std::size_t warmup = 10,
	std::size_t iters = 50) {
	if (!should_run(name)) {
		return;
	}
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto json_str = doc->dump();
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(); }, warmup, iters, 1, json_str->size());
	print_row(name, s);
}
Document make_string_document(
	std::string_view value) {
	auto b = value_builder();
	if (auto ok = b.set_string(value); !ok) {
		throw std::runtime_error{ok.error().message};
	}
	auto doc = std::move(b).finish();
	if (!doc) {
		throw std::runtime_error{doc.error().message};
	}
	return std::move(*doc);
}
void bench_string_escape_named(
	std::string_view label,
	std::string const &value,
	std::size_t warmup,
	std::size_t iters) {
	std::string const fallback_name = std::format("string_escape/fallback/{}", label);
	std::string const dump_name = std::format("string_escape/json_dump/{}", label);
	if (should_run(fallback_name)) {
		auto const expected = conflux::utils::json_string_fallback(value);
		auto s = measure(
			[&] {
				auto escaped = conflux::utils::json_string_fallback(value);
				bench_consume(escaped);
			},
			warmup,
			iters,
			1,
			expected.size());
		print_row(fallback_name, s);
	}
	if (should_run(dump_name)) {
		auto const doc = make_string_document(value);
		auto expected = doc.dump();
		if (!expected) {
			throw std::runtime_error{expected.error().message};
		}
		auto s = measure(
			[&] {
				auto dumped = doc.dump();
				bench_consume(dumped);
			},
			warmup,
			iters,
			1,
			expected->size());
		print_row(dump_name, s);
	}
}

struct CorpusFileSpec {
	std::string_view label;
	std::string_view file;
	std::size_t warmup{10};
	std::size_t iters{50};
	bool dump{true};
};

[[nodiscard]] std::filesystem::path corpus_root() {
	return std::filesystem::path{__FILE__}.parent_path() / "corpus";
}
[[nodiscard]] std::optional<std::string> load_corpus_file(
	std::string_view filename) {
	std::filesystem::path const p = corpus_root() / std::filesystem::path{std::string{filename}};
	std::ifstream f{p, std::ios::binary};
	if (!f) {
		return std::nullopt;
	}
	std::ostringstream out;
	out << f.rdbuf();
	return out.str();
}
void bench_parse_required_named(
	std::string_view name,
	std::string const &corpus,
	std::size_t warmup = 10,
	std::size_t iters = 50,
	JsonParseOptions const &opts = {}) {
	if (!should_run(name)) {
		return;
	}
	auto s = measure(
		[&] {
			auto doc = parse(corpus, opts);
			if (!doc) {
				throw std::runtime_error{"json benchmark fixture parse failed"};
			}
		},
		warmup,
		iters,
		1,
		corpus.size());
	print_row(name, s);
}
void bench_parse_reject_named(
	std::string_view name,
	std::string const &corpus,
	std::size_t warmup = 20,
	std::size_t iters = 100) {
	if (!should_run(name)) {
		return;
	}
	auto s = measure(
		[&] {
			auto doc = parse(corpus);
			if (doc) {
				throw std::runtime_error{"malformed JSON benchmark fixture parsed successfully"};
			}
		},
		warmup,
		iters,
		1,
		corpus.size());
	print_row(name, s);
}
void bench_file_corpora(
	std::string_view title,
	std::span<CorpusFileSpec const> specs) {
	bool printed_header = false;
	for (CorpusFileSpec const &spec: specs) {
		auto corpus = load_corpus_file(spec.file);
		if (!corpus) {
			continue;
		}
		if (!printed_header && !g_csv) {
			std::println("[json-bench]");
			std::println("[json-bench] -- {} --", title);
			printed_header = true;
		}
		bench_parse_required_named(std::format("parse/{}", spec.label), *corpus, spec.warmup, spec.iters);
		if (spec.dump) {
			bench_dump_named(std::format("dump/{}", spec.label), *corpus, spec.warmup, spec.iters);
		}
	}
}
void bench_reject_file_corpora(
	std::string_view title,
	std::span<CorpusFileSpec const> specs) {
	bool printed_header = false;
	for (CorpusFileSpec const &spec: specs) {
		auto corpus = load_corpus_file(spec.file);
		if (!corpus) {
			continue;
		}
		if (!printed_header && !g_csv) {
			std::println("[json-bench]");
			std::println("[json-bench] -- {} --", title);
			printed_header = true;
		}
		bench_parse_reject_named(std::format("reject/{}", spec.label), *corpus, spec.warmup, spec.iters);
	}
}
void bench_duplicate_policy_fixture(
	std::string const &corpus) {
	JsonParseOptions opts;
	opts.duplicate_key = DuplicateKeyPolicy::last_wins;
	bench_parse_required_named("parse/edge/duplicate_keys last_wins", corpus, 40, 200, opts);
	bench_parse_reject_named("reject/edge/duplicate_keys default", corpus, 40, 200);
}
// FI-1 — measures two components that together show the value of the sentinel:
//
//   (A) linear-only lookup (7-member, below kHashThreshold=48) — this is the
//       per-lookup cost WITH the sentinel cached (find_member short-circuits
//       straight to linear scan on every subsequent call after the first failure).
//
//   (B) hash-build overhead = (parse + first find_member) − (parse only), measured
//       on the 1024-member corpus. In the adversarial repeat-lookup scenario
//       WITHOUT the sentinel, (B) would be paid on every single call because
//       hash_idx_raw stays nullptr and each call retries alloc + build + free.
//       With the sentinel (FI-1), (B) is paid exactly once.
void bench_fi1_sentinel(
	std::string const &small_corpus,
	std::string const &lookup_corpus) {
	if (should_run("FI-1/sentinel: (A) linear-only 7-member (failure path proxy)")) {
		auto doc = parse(small_corpus);
		if (!doc) {
			return;
		}
		auto obj = doc->root().as_object();
		if (!obj) {
			return;
		}
		auto s = measure(
			[&] {
				(void)obj->find_member("field_0");
				(void)obj->find_member("field_3");
				(void)obj->find_member("field_6");
			},
			200,
			1000,
			1000);
		s.ns_per_iter /= 3.0;
		print_row("FI-1/sentinel: (A) linear-only 7-member (failure path proxy)", s);
	}
	if (should_run("FI-1/sentinel: (B) build+lookup overhead (parse+find − parse-only)")) {
		auto parse_only = measure([&] { (void)parse(lookup_corpus); }, 20, 100);
		auto parse_find = measure(
			[&] {
				auto d = parse(lookup_corpus);
				if (!d) {
					return;
				}
				auto o = d->root().as_object();
				if (!o) {
					return;
				}
				(void)o->find_member("member_512");
			},
			20,
			100);
		double const build_ns = parse_find.ns_per_iter - parse_only.ns_per_iter;
		BenchStats diff{};
		diff.ns_per_iter = std::max(0.0, build_ns);
		print_row("FI-1/sentinel: (B) build+lookup overhead (parse+find − parse-only)", diff);
	}
}

} // namespace
// ---------------------------------------------------------------------------
// UnknownMemberPolicy::reject cost on wide objects
// ---------------------------------------------------------------------------

struct BenchModel5 {
	std::int64_t id{};
	std::string name{};
	double score{};
	bool active{};
	std::string tag{};
};
template<>
struct conflux::json::JsonMembers<BenchModel5> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &BenchModel5::id),
			json_member("name", &BenchModel5::name),
			json_member("score", &BenchModel5::score),
			json_member("active", &BenchModel5::active),
			json_member("tag", &BenchModel5::tag),
		};
	}
	static constexpr std::string_view type_name() { return "BenchModel5"; }
};
namespace {

std::string make_reject_corpus(
	std::size_t extra_members) {
	std::string out;
	out.reserve(extra_members * 30 + 128);
	out += R"({"id":42,"name":"bench","score":3.14,"active":true,"tag":"x")";
	for (std::size_t i = 0; i < extra_members; ++i) {
		out += std::format(R"(,"extra_field_{}":{})", i, i);
	}
	out += '}';
	return out;
}
void bench_reject_policy() {
	for (std::size_t extra: std::array<std::size_t, 5>{0, 10, 50, 100, 200}) {
		std::string const reject_name = std::format("decode/reject 5+{} members", extra);
		std::string const ignore_name = std::format("decode/ignore 5+{} members", extra);
		if (!should_run(reject_name) && !should_run(ignore_name)) {
			continue;
		}
		std::string const corpus = make_reject_corpus(extra);
		auto doc_res = parse(corpus);
		if (!doc_res) {
			return;
		}

		if (should_run(reject_name)) {
			// reject policy (default): O(N·M) scan after DOM decode
			auto s_reject = measure(
				[&] {
					auto d = parse(corpus);
					if (!d) {
						return;
					}
					JsonDecodeOptions opts;
					opts.unknown_members = UnknownMemberPolicy::reject;
					auto r = decode<BenchModel5>(d->root(), opts);
					(void)r;
				},
				100,
				500);
			print_row(reject_name, s_reject);
		}

		if (should_run(ignore_name)) {
			// ignore policy: no extra scan
			auto s_ignore = measure(
				[&] {
					auto d = parse(corpus);
					if (!d) {
						return;
					}
					JsonDecodeOptions opts;
					opts.unknown_members = UnknownMemberPolicy::ignore;
					auto r = decode<BenchModel5>(d->root(), opts);
					(void)r;
				},
				100,
				500);
			print_row(ignore_name, s_ignore);
		}
	}
}

} // namespace
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"json","parser":"standard","configs":[{"name":"default","extra":{"kind":"micro/user-space","case":"JSON parser coverage suite"},"target_ms":500,"max_iterations":1000,"calibration_iterations":4,"args":["--iterations","0","--warmup","0"]},{"name":"parse_large","extra":{"kind":"micro/user-space","case":"large nested JSON parse"},"target_ms":500,"max_iterations":1000,"calibration_iterations":4,"args":["--filter","parse/large (","--config-name","parse_large","--iterations","0","--warmup","0"]},{"name":"parse_long_strings","extra":{"kind":"micro/user-space","case":"large borrowed-string JSON parse"},"target_ms":500,"max_iterations":1000,"calibration_iterations":4,"args":["--filter","parse/long_strings (","--config-name","parse_long_strings","--iterations","0","--warmup","0"]},{"name":"parse_escape_heavy","extra":{"kind":"micro/user-space","case":"escape-heavy JSON parse"},"target_ms":500,"max_iterations":5000,"calibration_iterations":4,"args":["--filter","parse/escape_heavy (","--config-name","parse_escape_heavy","--iterations","0","--warmup","0"]}],"filters":["--filter SUBSTR"]})");
	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	g_args = cfg;
	g_csv = cfg.json_out;
	g_filters = cfg.filters;
	if (!g_csv) {
		std::println("[json-bench] building corpora…");
	}
	std::string const config_corpus = make_config_corpus();
	std::string const decode_corpus = make_decode_corpus();
	std::string const lookup_corpus = make_lookup_corpus();
	std::string const array_corpus = make_array_corpus();
	std::string const large_corpus = make_large_corpus();
	std::string const long_strings_corpus = make_long_strings_corpus();
	std::string const pretty_ws_corpus = make_pretty_ws_corpus();
	std::string const escape_heavy_corpus = make_escape_heavy_corpus();
	std::string const plain_string_payload = make_plain_string_payload();
	std::string const escape_heavy_string_payload = make_escape_heavy_string_payload();
	std::string const deep_nest_corpus = make_deep_nest_corpus();
	std::string const mixed_numbers_corpus = make_mixed_numbers_corpus();
	std::string const lookup_escaped_corpus = make_lookup_escaped_corpus();
	std::string const lookup_mixed_corpus = make_lookup_mixed_corpus();
	std::string const below_threshold_corpus = make_below_threshold_corpus();
	std::string const linear31_corpus = make_linear31_corpus();

	if (!g_csv) {
		std::println(
			"[json-bench] corpus sizes: config={}B decode={}B lookup={}B A={}B large={}B",
			config_corpus.size(),
			decode_corpus.size(),
			lookup_corpus.size(),
			array_corpus.size(),
			large_corpus.size());
		std::println(
			"[json-bench]                long_strings={}B pretty_ws={}B escape_heavy={}B deep_nest={}B "
			"mixed_numbers={}B",
			long_strings_corpus.size(),
			pretty_ws_corpus.size(),
			escape_heavy_corpus.size(),
			deep_nest_corpus.size(),
			mixed_numbers_corpus.size());
		std::println("[json-bench]");
		std::println("[json-bench] {:<40} {:>10}     {:>10}", "benchmark", "median", "throughput");
		std::println("[json-bench] {}", std::string(60, '-'));
	}

	bench_parse_small(config_corpus);
	bench_parse_large(large_corpus);
	bench_decode(decode_corpus);
	bench_find_member(lookup_corpus);
	bench_array_traversal(array_corpus);
	bench_builder();
	bench_dump_plain(config_corpus);
	bench_dump_sorted(config_corpus);
	bench_accumulate_chunked("accumulate/byte_span chunked (4KB config)", config_corpus, 4096);

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- direct struct serde matrix --");
	}
	bench_direct_struct_matrix();

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- direct typed wide-object lookup matrix --");
	}
	bench_direct_wide_object_matrix();

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- R0 corpora (added v16) --");
	}
	bench_parse_named("parse/long_strings (1MB / 32x32KiB)", long_strings_corpus);
	bench_dump_named("dump/long_strings", long_strings_corpus);
	bench_parse_named("parse/pretty_ws (1MB indented)", pretty_ws_corpus);
	bench_dump_named("dump/pretty_ws", pretty_ws_corpus);
	bench_parse_named("parse/escape_heavy (256KiB)", escape_heavy_corpus, 20, 100);
	bench_dump_named("dump/escape_heavy", escape_heavy_corpus, 20, 100);
	bench_string_escape_named("plain_1MiB", plain_string_payload, 10, 50);
	bench_string_escape_named("escape_heavy_256KiB", escape_heavy_string_payload, 20, 100);
	bench_parse_named("parse/deep_nest (256 levels)", deep_nest_corpus, 100, 500);
	bench_parse_named("parse/mixed_numbers (1MB)", mixed_numbers_corpus);
	bench_dump_named("dump/mixed_numbers", mixed_numbers_corpus);
	bench_accumulate_chunked("accumulate/byte_span chunked (1MB large)", large_corpus, 4096);
	if (!g_csv) {
		std::println("[json-bench]");
		std::println(
			"[json-bench] -- e2e JSON decode: conflux::file_io::FileReader vs conflux::socket_io::SocketTaskRing --");
	}
	bench_e2e_decode("e2e/large_json_decode", large_corpus);

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- v16 Item C/E: member_name dispatch + builder name-copy --");
		std::println("[json-bench]    Baseline (plain names, kStorageInputView) already shown above.");
	}
	bench_find_member_escaped(lookup_escaped_corpus);
	bench_find_member_mixed(lookup_mixed_corpus);
	bench_find_member_linear31(linear31_corpus);
	bench_builder_name_length();

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- FI-1: sentinel prevents repeated hash-build on failure --");
		std::println("[json-bench]    (A) per-lookup cost after sentinel cached; (B) overhead saved per repeat call");
		std::println(
			"[json-bench]    adversarial cost WITHOUT sentinel: (A)+(B) per lookup; WITH: (A) after first call");
	}
	bench_fi1_sentinel(below_threshold_corpus, lookup_corpus);

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] -- UnknownMemberPolicy::reject O(N·M) cost --");
	}
	bench_reject_policy();

	if (!g_csv) {
		std::println("[json-bench]");
		std::println("[json-bench] Acceptance thresholds:");
		std::println("[json-bench]   parse >=500 MB/s on typical-config corpus");
		std::println("[json-bench]   find_member <=1000 ns median on 1024-member object");
		std::println("[json-bench]   dump >=1000 MB/s on plain path");
	}

	{
		std::array<CorpusFileSpec, 5> const real_world{
			{
             {"file/canada geo", "canada.json"},
             {"file/citm_catalog catalog", "citm_catalog.json"},
             {"file/twitter social", "twitter.json"},
             {"file/apache_builds CI", "apache_builds.json"},
             {"file/github_events events", "github_events.json"},
			 }
        };
		bench_file_corpora("real-world corpora", real_world);
	}
	{
		std::array<CorpusFileSpec, 4> const route_payloads{
			{
             {"route/persona_create_request", "route_payloads/persona_create_request.json", 20, 200},
             {"route/content_generation_response", "route_payloads/content_generation_response.json", 20, 200},
             {"route/scheduled_publish_batch", "route_payloads/scheduled_publish_batch.json", 20, 200},
             {"route/analytics_timeseries", "route_payloads/analytics_timeseries.json", 20, 200},
			 }
        };
		bench_file_corpora("route payload fixtures", route_payloads);
	}
	{
		std::array<CorpusFileSpec, 3> const edge_cases{
			{
             {"edge/large_numbers", "edge/large_numbers.json", 20, 200},
             {"edge/escaped_unicode", "edge/escaped_unicode.json", 20, 200},
             {"edge/out_of_order_keys", "edge/out_of_order_keys.json", 20, 200},
			 }
        };
		bench_file_corpora("edge-case valid fixtures", edge_cases);
		if (auto duplicate_keys = load_corpus_file("edge/duplicate_keys.json")) {
			if (!g_csv) {
				std::println("[json-bench]");
				std::println("[json-bench] -- duplicate-key policy fixture --");
			}
			bench_duplicate_policy_fixture(*duplicate_keys);
		}
	}
	{
		std::array<CorpusFileSpec, 5> const malformed{
			{
             {"malformed/trailing_comma", "malformed/trailing_comma.json", 20, 200, false},
             {"malformed/bad_string_escape", "malformed/bad_string_escape.json", 20, 200, false},
             {"malformed/leading_zero", "malformed/leading_zero.json", 20, 200, false},
             {"malformed/unclosed_array", "malformed/unclosed_array.json", 20, 200, false},
             {"malformed/garbage_suffix", "malformed/garbage_suffix.json", 20, 200, false},
			 }
        };
		bench_reject_file_corpora("malformed rejection fixtures", malformed);
	}
}
