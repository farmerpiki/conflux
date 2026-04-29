// libFuzzer driver for ws_accept_key.
// Pure function; invariant = doesn't crash on any byte stream.
// Accept key is a fixed-length base64-encoded SHA-1, so bound the output.

import std;
import conflux.types;
import conflux.net.router;

using namespace std;

extern "C" int LLVMFuzzerTestOneInput(
	u8 const *data,
	SZ size) {
	if (size > 4096) {
		return 0;
	}
	SV in{reinterpret_cast<char const *>(data), size};
	auto const out = ws_detail::ws_accept_key(in);
	if (out.size() != 28) {
		__builtin_trap();
	}
	return 0;
}
