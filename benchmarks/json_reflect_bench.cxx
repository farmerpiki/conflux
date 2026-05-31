import std;
import conflux.json;
import conflux.json.boundary;
import conflux.json.reflect;
import conflux.json.reflect_provider;
import bench_common;

using namespace conflux::json;

namespace {

std::atomic<bool> g_count_allocations{false};
std::atomic<std::uint64_t> g_alloc_count{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};

} // namespace

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
void operator delete(
	void *p) noexcept {
	std::free(p);
}
void operator delete[](
	void *p) noexcept {
	::operator delete(p);
}
void operator delete(
	void *p,
	std::size_t) noexcept {
	::operator delete(p);
}
void operator delete[](
	void *p,
	std::size_t) noexcept {
	::operator delete(p);
}

namespace {

struct AllocBenchStats {
	BenchStats timing;
	double allocations_per_iter{};
	double allocated_bytes_per_iter{};
};

template<typename F>
AllocBenchStats measure_alloc(
	F &&fn,
	BenchSamplePlan const &plan,
	std::size_t bytes = 0) {
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
	double const med = static_cast<double>(samples[plan.samples / 2]) / static_cast<double>(plan.batch);
	double const mbs = (bytes > 0 && med > 0.0) ? static_cast<double>(bytes) / (med / 1e9) / (1024.0 * 1024.0) : 0.0;
	double const denom = static_cast<double>(plan.iterations);
	BenchStats timing{
		.iterations = plan.iterations,
		.total_ns = total,
		.ns_per_iter = med,
		.throughput = mbs,
	};
	bench_apply_sample_plan(timing, plan);
	return {
		.timing = timing,
		.allocations_per_iter = static_cast<double>(total_allocs) / denom,
		.allocated_bytes_per_iter = static_cast<double>(total_bytes) / denom,
	};
}

bool g_json = false;
BenchArgs g_args;

[[nodiscard]] BenchSamplePlan make_plan(
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch = 1) {
	return bench_sample_plan(g_args, iters, warmup, batch);
}

template<typename F>
AllocBenchStats measure_alloc(
	F &&fn,
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch = 1,
	std::size_t bytes = 0) {
	return measure_alloc(std::forward<F>(fn), make_plan(warmup, iters, batch), bytes);
}
void print_alloc_row(
	std::string_view name,
	AllocBenchStats s) {
	s.timing.variant = name;
	if (g_json) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"sample_count\":{},\"batch\":{},\"timer_sample_ns\":{},\"timer_overhead_pct\":{:.4f},"
			"\"allocations_per_iter\":{:.2f},\"allocated_bytes_per_iter\":{:.2f}}}",
			s.timing.config,
			s.timing.variant,
			s.timing.iterations,
			s.timing.total_ns,
			s.timing.ns_per_iter,
			s.timing.sample_count,
			s.timing.batch,
			s.timing.timer_sample_ns,
			s.timing.timer_overhead_pct,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter);
	} else {
		std::println(
			"[json-reflect-bench] {:<36} {:>10.1f} ns  {:>8.1f} MB/s  {:>6.2f} allocs  {:>8.1f} B  [{}×{} "
			"timer≈{:.2f}%]",
			name,
			s.timing.ns_per_iter,
			s.timing.throughput,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter,
			s.timing.sample_count,
			s.timing.batch,
			s.timing.timer_overhead_pct);
	}
}

struct ReflectSmall {
	std::int64_t id{};
	bool active{};
};
struct ReflectMedium {
	std::int64_t id{};
	std::int64_t count{};
	double score{};
	bool active{};
	std::string name{};
	std::string tag{};
	std::optional<std::int64_t> limit{};
	std::vector<std::int64_t> values{};
};
struct ReflectWide16 {
	std::int64_t f00{};
	std::int64_t f01{};
	std::int64_t f02{};
	std::int64_t f03{};
	std::int64_t f04{};
	std::int64_t f05{};
	std::int64_t f06{};
	std::int64_t f07{};
	std::int64_t f08{};
	std::int64_t f09{};
	std::int64_t f10{};
	std::int64_t f11{};
	std::int64_t f12{};
	std::int64_t f13{};
	std::int64_t f14{};
	std::int64_t f15{};
};

template<class T>
void require_decode(
	std::expected<T, JsonError> value) {
	if (!value) {
		throw std::runtime_error{value.error().message};
	}
}
template<class T>
void require_decode_reject(
	std::expected<T, JsonError> value) {
	if (value) {
		throw std::runtime_error{"expected reflected direct decode to reject benchmark fixture"};
	}
}
template<class T>
void require_boundary_decode(
	std::expected<T, conflux::json::boundary::Error> value) {
	if (!value) {
		throw std::runtime_error{value.error().message};
	}
}
void require_boundary_dump(
	std::expected<std::string, conflux::json::boundary::Error> value) {
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

[[nodiscard]] std::string make_reflect_medium_reordered() {
	return R"({"values":[1,2,3,4,5,6,7,8],"limit":64,"tag":"direct","name":"bench","active":true,"score":12.5,"count":42,"id":7})";
}
[[nodiscard]] std::string make_reflect_medium_shuffled() {
	return R"({"score":12.5,"name":"bench","id":7,"values":[1,2,3,4,5,6,7,8],"active":true,"limit":64,"count":42,"tag":"direct"})";
}
[[nodiscard]] std::string make_reflect_medium_duplicate_name() {
	return R"({"id":7,"count":42,"score":12.5,"active":true,"name":"first","name":"last","tag":"direct","limit":64,"values":[1,2,3,4]})";
}
[[nodiscard]] std::string make_reflect_medium_duplicate_vector() {
	return R"({"id":7,"count":42,"score":12.5,"active":true,"name":"bench","tag":"direct","limit":64,"values":[1,2,3,4],"values":[9,8,7]})";
}
[[nodiscard]] std::string make_reflect_medium_unknown_duplicate() {
	return R"({"id":7,"count":42,"score":12.5,"active":true,"extra":1,"extra":2,"name":"bench","tag":"direct","limit":64,"values":[1,2,3,4]})";
}
[[nodiscard]] std::string make_reflect_medium_json5_duplicate() {
	return R"({id:7,count:42,score:12.5,active:true,name:'first',name:'last',tag:'direct',limit:64,values:[1,2,3,4,],})";
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"json_reflect","parser":"standard","configs":[{"name":"p2996","extra":{"reflection":true},"args":[]}],"filters":["--filter SUBSTR"]})");
	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	g_args = cfg;
	g_json = cfg.json_out;

	using Provider = conflux::json::boundary::NativeReflectJsonProvider;
	std::string const small = R"({"id":7,"active":true})";
	std::string const medium =
		R"({"id":7,"count":42,"score":12.5,"active":true,"name":"bench","tag":"direct","limit":64,"values":[1,2,3,4,5,6,7,8]})";
	std::string const medium_reordered = make_reflect_medium_reordered();
	std::string const medium_shuffled = make_reflect_medium_shuffled();
	std::string const medium_duplicate_name = make_reflect_medium_duplicate_name();
	std::string const medium_duplicate_vector = make_reflect_medium_duplicate_vector();
	std::string const medium_unknown_duplicate = make_reflect_medium_unknown_duplicate();
	std::string const medium_json5_duplicate = make_reflect_medium_json5_duplicate();
	std::string const wide16 =
		R"({"f00":0,"f01":1,"f02":2,"f03":3,"f04":4,"f05":5,"f06":6,"f07":7,"f08":8,"f09":9,"f10":10,"f11":11,"f12":12,"f13":13,"f14":14,"f15":15})";
	ReflectMedium const medium_value{
		.id = 7,
		.count = 42,
		.score = 12.5,
		.active = true,
		.name = "bench",
		.tag = "direct",
		.limit = 64,
		.values = {1, 2, 3, 4, 5, 6, 7, 8},
	};
	conflux::json::boundary::DumpOptions sorted;
	sorted.sort_object_keys = true;
	JsonParseOptions duplicate_reject;
	duplicate_reject.duplicate_key = DuplicateKeyPolicy::reject;
	JsonParseOptions duplicate_first;
	duplicate_first.duplicate_key = DuplicateKeyPolicy::first_wins;
	JsonParseOptions duplicate_last;
	duplicate_last.duplicate_key = DuplicateKeyPolicy::last_wins;
	JsonParseOptions json5_duplicate_reject = duplicate_reject;
	json5_duplicate_reject.mode = ParseMode::json5;
	JsonParseOptions json5_duplicate_last = duplicate_last;
	json5_duplicate_last.mode = ParseMode::json5;
	JsonDecodeOptions ignore_unknown;
	ignore_unknown.unknown_members = UnknownMemberPolicy::ignore;

	if (!g_json) {
		std::println("[json-reflect-bench] benchmark                         median      throughput     allocations");
		std::println(
			"[json-reflect-bench] -----------------------------------------------------------------------------");
	}
	auto run = [&](std::string_view name, auto &&fn) {
		if (bench_matches_filter(cfg, name)) {
			print_alloc_row(name, fn());
		}
	};
	run("decode/reflection/dom/small", [&] {
		return measure_alloc(
			[&] {
				auto doc = parse(small);
				if (!doc) {
					throw std::runtime_error{doc.error().message};
				}
				require_decode(decode<ReflectSmall>(doc->root()));
			},
			100,
			500,
			1,
			small.size());
	});
	run("decode/reflection/direct/small", [&] {
		return measure_alloc(
			[&] { require_boundary_decode(Provider::decode_json<ReflectSmall>(small, {.copy_input = false})); },
			100,
			500,
			1,
			small.size());
	});
	run("decode/reflection/direct/medium", [&] {
		return measure_alloc(
			[&] { require_boundary_decode(Provider::decode_json<ReflectMedium>(medium, {.copy_input = false})); },
			100,
			500,
			1,
			medium.size());
	});
	run("decode/reflection/direct/medium/out_of_order_reverse", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<ReflectMedium>(medium_reordered)); },
			100,
			500,
			1,
			medium_reordered.size());
	});
	run("decode/reflection/direct/medium/out_of_order_shuffled", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<ReflectMedium>(medium_shuffled)); },
			100,
			500,
			1,
			medium_shuffled.size());
	});
	run("decode/reflection/direct/medium/duplicate_name_reject", [&] {
		return measure_alloc(
			[&] { require_decode_reject(decode_borrowed<ReflectMedium>(medium_duplicate_name, duplicate_reject)); },
			100,
			500,
			1,
			medium_duplicate_name.size());
	});
	run("decode/reflection/direct/medium/duplicate_name_first_wins", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<ReflectMedium>(medium_duplicate_name, duplicate_first)); },
			100,
			500,
			1,
			medium_duplicate_name.size());
	});
	run("decode/reflection/direct/medium/duplicate_name_last_wins", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<ReflectMedium>(medium_duplicate_name, duplicate_last)); },
			100,
			500,
			1,
			medium_duplicate_name.size());
	});
	run("decode/reflection/direct/medium/duplicate_vector_first_wins", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<ReflectMedium>(medium_duplicate_vector, duplicate_first)); },
			100,
			500,
			1,
			medium_duplicate_vector.size());
	});
	run("decode/reflection/direct/medium/duplicate_vector_last_wins", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<ReflectMedium>(medium_duplicate_vector, duplicate_last)); },
			100,
			500,
			1,
			medium_duplicate_vector.size());
	});
	run("decode/reflection/direct/medium/duplicate_unknown_ignore", [&] {
		return measure_alloc(
			[&] {
				require_decode(
					decode_borrowed<ReflectMedium>(medium_unknown_duplicate, duplicate_reject, ignore_unknown));
			},
			100,
			500,
			1,
			medium_unknown_duplicate.size());
	});
	run("decode/reflection/direct/medium/json5_duplicate_reject", [&] {
		return measure_alloc(
			[&] {
				require_decode_reject(decode_borrowed<ReflectMedium>(medium_json5_duplicate, json5_duplicate_reject));
			},
			100,
			500,
			1,
			medium_json5_duplicate.size());
	});
	run("decode/reflection/direct/medium/json5_duplicate_last_wins", [&] {
		return measure_alloc(
			[&] { require_decode(decode_borrowed<ReflectMedium>(medium_json5_duplicate, json5_duplicate_last)); },
			100,
			500,
			1,
			medium_json5_duplicate.size());
	});
	run("decode/reflection/direct/wide16", [&] {
		return measure_alloc(
			[&] { require_boundary_decode(Provider::decode_json<ReflectWide16>(wide16, {.copy_input = false})); },
			100,
			500,
			1,
			wide16.size());
	});
	run("write/reflection/dom", [&] {
		return measure_alloc(
			[&] { require_boundary_dump(Provider::dump_json(medium_value, sorted)); },
			100,
			500,
			1,
			medium.size());
	});
	run("write/reflection/direct", [&] {
		return measure_alloc([&] { require_dump(dump_reflect_direct(medium_value)); }, 100, 500, 1, medium.size());
	});
}
