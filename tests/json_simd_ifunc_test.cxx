#include <cstddef>
#include <cstring>
#include <iostream>

extern "C" {
using conflux_json_scan_str_fn = std::size_t (*)(char const *, std::size_t) noexcept;
using conflux_json_scan_dump_fn = std::size_t (*)(char const *, std::size_t, int) noexcept;

std::size_t conflux_json_scan_str_until_special_stdsimd(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd(char const *, std::size_t, int) noexcept;

std::size_t conflux_json_scan_str_until_special_stdsimd_avx2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd_avx2(char const *, std::size_t, int) noexcept;
std::size_t conflux_json_scan_str_until_special_stdsimd_sse2(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd_sse2(char const *, std::size_t, int) noexcept;
std::size_t conflux_json_scan_str_until_special_stdsimd_scalar(char const *, std::size_t) noexcept;
std::size_t conflux_json_scan_dump_safe_run_stdsimd_scalar(char const *, std::size_t, int) noexcept;

conflux_json_scan_str_fn resolve_conflux_json_scan_str_until_special() noexcept;
conflux_json_scan_dump_fn resolve_conflux_json_scan_dump_safe_run() noexcept;
}

namespace {

struct expected_variant {
	conflux_json_scan_str_fn str;
	conflux_json_scan_dump_fn dump;
	bool executable;
};

bool cpu_supports_avx2() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
	__builtin_cpu_init();
	return __builtin_cpu_supports("avx2") != 0;
#else
	return false;
#endif
}

bool cpu_supports_sse2() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
	return true;
#elif defined(__i386__) || defined(_M_IX86)
	__builtin_cpu_init();
	return __builtin_cpu_supports("sse2") != 0;
#else
	return false;
#endif
}

expected_variant expected_for(
	char const *variant) {
	if (std::strcmp(variant, "avx2") == 0) {
		return {
			conflux_json_scan_str_until_special_stdsimd_avx2,
			conflux_json_scan_dump_safe_run_stdsimd_avx2,
			cpu_supports_avx2()};
	}
	if (std::strcmp(variant, "sse2") == 0) {
		return {
			conflux_json_scan_str_until_special_stdsimd_sse2,
			conflux_json_scan_dump_safe_run_stdsimd_sse2,
			cpu_supports_sse2()};
	}
	if (std::strcmp(variant, "scalar") == 0) {
		return {
			conflux_json_scan_str_until_special_stdsimd_scalar,
			conflux_json_scan_dump_safe_run_stdsimd_scalar,
			true};
	}
	return {nullptr, nullptr, false};
}

} // namespace

int main(
	int argc,
	char **argv) {
	if (argc != 2) {
		std::cerr << "usage: " << argv[0] << " scalar|sse2|avx2\n";
		return 2;
	}

	auto const expected = expected_for(argv[1]);
	if (expected.str == nullptr || expected.dump == nullptr) {
		std::cerr << "unknown variant: " << argv[1] << '\n';
		return 2;
	}

	auto const resolved_str = resolve_conflux_json_scan_str_until_special();
	auto const resolved_dump = resolve_conflux_json_scan_dump_safe_run();
	if (resolved_str != expected.str) {
		std::cerr << "string scan resolver selected the wrong variant for " << argv[1] << '\n';
		return 1;
	}
	if (resolved_dump != expected.dump) {
		std::cerr << "dump scan resolver selected the wrong variant for " << argv[1] << '\n';
		return 1;
	}

	if (!expected.executable) {
		return 0;
	}

	char const str_input[] = "abc\ndef";
	if (conflux_json_scan_str_until_special_stdsimd(str_input, sizeof(str_input) - 1) != 3) {
		std::cerr << "string scan returned the wrong result for " << argv[1] << '\n';
		return 1;
	}

	char const dump_input[] = "abc\"def";
	if (conflux_json_scan_dump_safe_run_stdsimd(dump_input, sizeof(dump_input) - 1, 0) != 3) {
		std::cerr << "dump scan returned the wrong result for " << argv[1] << '\n';
		return 1;
	}

	return 0;
}
