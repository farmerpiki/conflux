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
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch = 1,
	std::size_t bytes = 0) {
	for (std::size_t i = 0; i < warmup * batch; ++i) {
		fn();
	}
	std::vector<std::uint64_t> samples;
	samples.reserve(iters);
	std::uint64_t total = 0;
	std::uint64_t total_allocs = 0;
	std::uint64_t total_bytes = 0;
	for (std::size_t i = 0; i < iters; ++i) {
		g_alloc_count.store(0, std::memory_order_relaxed);
		g_alloc_bytes.store(0, std::memory_order_relaxed);
		g_count_allocations.store(true, std::memory_order_relaxed);
		std::uint64_t const t0 = bench_now_ns();
		for (std::size_t j = 0; j < batch; ++j) {
			fn();
		}
		std::uint64_t const elapsed = bench_now_ns() - t0;
		g_count_allocations.store(false, std::memory_order_relaxed);
		total += elapsed;
		total_allocs += g_alloc_count.load(std::memory_order_relaxed);
		total_bytes += g_alloc_bytes.load(std::memory_order_relaxed);
		samples.push_back(elapsed);
	}
	sort(samples.begin(), samples.end());
	double const med = static_cast<double>(samples[iters / 2]) / static_cast<double>(batch);
	double const mbs = (bytes > 0 && med > 0.0) ? static_cast<double>(bytes) / (med / 1e9) / (1024.0 * 1024.0) : 0.0;
	double const denom = static_cast<double>(iters * batch);
	return {
		.timing =
			{
					 .iterations = iters * batch,
					 .total_ns = total,
					 .ns_per_iter = med,
					 .throughput = mbs,
					 },
		.allocations_per_iter = static_cast<double>(total_allocs) / denom,
		.allocated_bytes_per_iter = static_cast<double>(total_bytes) / denom,
	};
}

bool g_json = false;
void print_alloc_row(
	std::string_view name,
	AllocBenchStats s) {
	s.timing.variant = name;
	if (g_json) {
		std::println(
			"{{\"config\":\"{}\",\"variant\":\"{}\",\"iterations\":{},\"total_ns\":{},\"ns_per_iter\":{:.2f},"
			"\"allocations_per_iter\":{:.2f},\"allocated_bytes_per_iter\":{:.2f}}}",
			s.timing.config,
			s.timing.variant,
			s.timing.iterations,
			s.timing.total_ns,
			s.timing.ns_per_iter,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter);
	} else {
		std::println(
			"[json-reflect-bench] {:<36} {:>10.1f} ns  {:>8.1f} MB/s  {:>6.2f} allocs  {:>8.1f} B",
			name,
			s.timing.ns_per_iter,
			s.timing.throughput,
			s.allocations_per_iter,
			s.allocated_bytes_per_iter);
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
	std::int64_t value_a{};
	std::int64_t value_b{};
};

template<class T>
void require_decode(
	std::expected<T, JsonError> value) {
	if (!value) {
		throw std::runtime_error{value.error().message};
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

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"json_reflect","parser":"standard","configs":[{"name":"p2996","extra":{"reflection":true},"args":[]}]})");
	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	g_json = cfg.json_out;

	using Provider = conflux::json::boundary::NativeReflectJsonProvider;
	std::string const small = R"({"id":7,"active":true})";
	std::string const medium =
		R"({"id":7,"count":42,"score":12.5,"active":true,"name":"bench","tag":"direct","limit":64,"value_a":1,"value_b":2})";
	ReflectMedium const medium_value{
		.id = 7,
		.count = 42,
		.score = 12.5,
		.active = true,
		.name = "bench",
		.tag = "direct",
		.limit = 64,
		.value_a = 1,
		.value_b = 2,
	};
	conflux::json::boundary::DumpOptions sorted;
	sorted.sort_object_keys = true;

	if (!g_json) {
		std::println("[json-reflect-bench] benchmark                         median      throughput     allocations");
		std::println(
			"[json-reflect-bench] -----------------------------------------------------------------------------");
	}
	print_alloc_row(
		"decode/reflection/dom/small",
		measure_alloc(
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
			small.size()));
	print_alloc_row(
		"decode/reflection/direct/small",
		measure_alloc(
			[&] { require_boundary_decode(Provider::decode_json<ReflectSmall>(small, {.copy_input = false})); },
			100,
			500,
			1,
			small.size()));
	print_alloc_row(
		"decode/reflection/direct/medium",
		measure_alloc(
			[&] { require_boundary_decode(Provider::decode_json<ReflectMedium>(medium, {.copy_input = false})); },
			100,
			500,
			1,
			medium.size()));
	print_alloc_row(
		"write/reflection/dom",
		measure_alloc(
			[&] { require_boundary_dump(Provider::dump_json(medium_value, sorted)); },
			100,
			500,
			1,
			medium.size()));
	print_alloc_row(
		"write/reflection/direct",
		measure_alloc([&] { require_dump(dump_reflect_direct(medium_value)); }, 100, 500, 1, medium.size()));
}
