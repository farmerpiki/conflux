// libFuzzer driver for parse_urlencoded.
// Pure; invariant = doesn't crash on any byte stream up to reasonable cap.

import std;
import conflux.net.router;
import conflux.net.http_server;

using namespace std;

extern "C" int LLVMFuzzerTestOneInput(
	uint8_t const *data,
	size_t size) {
	if (size > 65536) {
		return 0;
	}
	string_view in{reinterpret_cast<char const *>(data), size};
	HttpFieldsView out;
	parse_urlencoded(in, out);
	return 0;
}
