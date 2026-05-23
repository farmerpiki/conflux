// libFuzzer driver for normalize_static_path.
// Invariants:
//   - accepted paths are absolute, canonicalized server-root-relative paths
//   - no NUL, repeated separators, dot segments, or traversal segments survive

import std;
import conflux.types;
import conflux.net.http.static_core;

using namespace std;

extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size > 64U * 1024U) {
		return 0;
	}
	std::string_view input{reinterpret_cast<char const *>(data), size};
	auto const normalized = normalize_static_path(input);
	if (!normalized) {
		return 0;
	}
	auto const &path = *normalized;
	if (!path.empty() && path.front() != '/') {
		__builtin_trap();
	}
	if (path.find('\0') != std::string::npos
		|| path.find("//") != std::string::npos
		|| path.find("/./") != std::string::npos
		|| path == "/."
		|| path.find("/../") != std::string::npos
		|| path == "/..") {
		__builtin_trap();
	}
	return 0;
}
