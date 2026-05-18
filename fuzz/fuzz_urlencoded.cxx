// libFuzzer driver for parse_urlencoded.
// Pure; invariant = doesn't crash on any byte stream up to reasonable cap.

import std;
import conflux.types;
import conflux.net.http.parse_helpers;

using namespace std;
extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size > 65536) {
		return 0;
	}
	std::string_view in{reinterpret_cast<char const *>(data), size};
	HttpFieldsView out;
	parse_urlencoded(in, out);
	return 0;
}
