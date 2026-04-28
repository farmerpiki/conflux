import std;
import conflux.db;

using namespace std;
using namespace conflux::db;

namespace {

atomic<uintptr_t> sink{};

void add_n(
	Params &p,
	size_t n) {
	for (size_t i = 0; i < n; ++i) {
		switch (i % 5) {
		case 0 : p.add(static_cast<int64_t>(i * 7 + 1)); break;
		case 1 : p.add(static_cast<double>(i) * 1.5); break;
		case 2 : p.add(string_view{"benchmark_string_value"}); break;
		case 3 : p.add(true); break;
		default: p.add_null(); break;
		}
	}
}

void bench(
	string_view label,
	size_t n_params,
	size_t iters) {
	auto const t0 = chrono::steady_clock::now();
	for (size_t i = 0; i < iters; ++i) {
		Params p;
		add_n(p, n_params);
		auto const *v = p.values();
		sink.fetch_add(reinterpret_cast<uintptr_t>(v), memory_order_relaxed);
	}
	auto const ns = chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now() - t0).count();
	println("{:<35}  {:6} ns/iter  ({} iters)", label, ns / static_cast<int64_t>(iters), iters);
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
	constexpr size_t kIters = 1'000'000;
	println("=== Params bind baseline (text, heap) ===");
	bench("params=1  (int64)", 1, kIters);
	bench("params=4  (i64,f64,str,bool)", 4, kIters);
	bench("params=16 (mixed)", 16, kIters);
	bench("params=64 (mixed)", 64, kIters);
	println("(sink={})", sink.load());
}
