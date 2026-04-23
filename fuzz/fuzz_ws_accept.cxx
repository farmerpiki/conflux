// libFuzzer driver for ws_accept_key.
// Pure function; invariant = doesn't crash on any byte stream.
// Accept key is a fixed-length base64-encoded SHA-1, so bound the output.

import std;
import conflux.net.router;

using namespace std;

extern "C" int LLVMFuzzerTestOneInput(
	uint8_t const *data,
	size_t size) {
	if (size > 4096) {
		return 0;
	}
	string_view in{reinterpret_cast<char const *>(data), size};
	auto const out = ws_detail::ws_accept_key(in);
	if (out.size() != 28) {
		__builtin_trap();
	}
	return 0;
}
