import std;
import conflux.types;
import conflux.work.root;
import bench_common;
#if CONFLUX_WORK_CARRIER_MODEL_A
import conflux.work.carrier;
#endif
#if CONFLUX_WORK_CARRIER_MODEL_B
import conflux.work.carrier.model_b;
#endif

// Compile-time instantiation cost probe.
// Instantiates a 10-stage mixed-type chain for each enabled model.
// Build with -j1 and measure elapsed time; do not run.

#if CONFLUX_WORK_CARRIER_MODEL_A
namespace carrier = conflux::work::carrier;
auto chain_a_10stage(
	carrier::Chain<int> c) -> carrier::Chain<double> {
	auto s1 = carrier::map(move(c), [](int v) { return v + 1; });
	auto s2 = carrier::map(move(s1), [](int v) { return v * 2; });
	auto s3 = carrier::map(move(s2), [](int v) { return static_cast<double>(v); });
	auto s4 = carrier::map(move(s3), [](double v) { return static_cast<int>(v); });
	auto s5 = carrier::map(move(s4), [](int v) { return v - 1; });
	auto s6 = carrier::map(move(s5), [](int v) { return v + 3; });
	auto s7 = carrier::map(move(s6), [](int v) { return static_cast<double>(v) * 1.5; });
	auto s8 = carrier::map(move(s7), [](double v) { return static_cast<int>(v); });
	auto s9 = carrier::map(move(s8), [](int v) { return v * v; });
	return carrier::map(move(s9), [](int v) { return static_cast<double>(v); });
}
#endif

#if CONFLUX_WORK_CARRIER_MODEL_B
namespace model_b = conflux::work::carrier::model_b;
auto chain_b_10stage(
	model_b::TaskChain<int> c) -> model_b::TaskChain<double> {
	auto s1 = model_b::map(move(c), [](int v) { return v + 1; });
	auto s2 = model_b::map(move(s1), [](int v) { return v * 2; });
	auto s3 = model_b::map(move(s2), [](int v) { return static_cast<double>(v); });
	auto s4 = model_b::map(move(s3), [](double v) { return static_cast<int>(v); });
	auto s5 = model_b::map(move(s4), [](int v) { return v - 1; });
	auto s6 = model_b::map(move(s5), [](int v) { return v + 3; });
	auto s7 = model_b::map(move(s6), [](int v) { return static_cast<double>(v) * 1.5; });
	auto s8 = model_b::map(move(s7), [](double v) { return static_cast<int>(v); });
	auto s9 = model_b::map(move(s8), [](int v) { return v * v; });
	return model_b::map(move(s9), [](int v) { return static_cast<double>(v); });
}
#endif
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(argc, argv, R"({"name":"work_compile","parser":"standard","configs":[]})");
}
