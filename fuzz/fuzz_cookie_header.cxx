// libFuzzer driver for parse_cookies.
// Invariants:
//   - never crash on arbitrary Cookie header bytes
//   - parsed cookie names/values remain borrowed from the original header
//   - parse output stays bounded by semicolon-delimited input shape

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.net.http_server_helpers;

using namespace std;

namespace {

[[nodiscard]] bool points_into(
	std::string_view haystack,
	std::string_view needle) noexcept {
	if (needle.empty()) {
		return true;
	}
	auto const *base = haystack.data();
	auto const *ptr = needle.data();
	return ptr >= base && ptr + needle.size() <= base + haystack.size();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size > 64U * 1024U) {
		return 0;
	}
	std::string_view input{reinterpret_cast<char const *>(data), size};
	HttpFieldsView cookies;
	parse_cookies(input, cookies);

	std::size_t max_fields = 1;
	for (char c: input) {
		if (c == ';') {
			++max_fields;
		}
	}
	if (cookies.size() > max_fields) {
		__builtin_trap();
	}
	for (auto const &[name, value]: cookies) {
		if (!points_into(input, name) || !points_into(input, value)) {
			__builtin_trap();
		}
	}
	return 0;
}
