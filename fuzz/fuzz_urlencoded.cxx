// libFuzzer driver for parse_urlencoded.
// Pure; invariant = doesn't crash on any byte stream up to reasonable cap.

import std;
import conflux.types;
import conflux.net.router;
import conflux.net.http_server;

using namespace std;
extern "C" int LLVMFuzzerTestOneInput(
	u8 const *data,
	SZ size) {
	if (size > 65536) {
		return 0;
	}
	SV in{reinterpret_cast<char const *>(data), size};
	HttpFieldsView out;
	parse_urlencoded(in, out);
	return 0;
}
