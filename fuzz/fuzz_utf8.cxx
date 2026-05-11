// libFuzzer driver for utf8_is_valid.
// Pure predicate: only invariant is "does not crash on any byte stream".

import std;
import conflux.types;
import conflux.net.router;

using namespace std;
extern "C" int LLVMFuzzerTestOneInput(
	u8 const *data,
	SZ size) {
	SV s{reinterpret_cast<char const *>(data), size};
	[[maybe_unused]] auto const r = ws_detail::utf8_is_valid(s);
	return 0;
}
