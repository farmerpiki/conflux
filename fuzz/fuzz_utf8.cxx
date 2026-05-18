// libFuzzer driver for utf8_is_valid.
// Pure predicate: only invariant is "does not crash on any byte stream".

import std;
import conflux.types;
import conflux.net.http.realtime;

using namespace std;
extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	std::string_view s{reinterpret_cast<char const *>(data), size};
	[[maybe_unused]] auto const r = ws_detail::utf8_is_valid(s);
	return 0;
}
