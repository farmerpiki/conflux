import std;
import conflux.types;
import conflux.json;

using namespace std;
using namespace conflux::json;

extern "C" int LLVMFuzzerTestOneInput(
	u8 const *data,
	SZ size) {
	if (size == 0) {
		return 0;
	}

	SV input{reinterpret_cast<char const *>(data), size};

	JsonParseOptions opts{.max_depth = LimitOption::bound(128)};
	NdjsonRange range{input, opts};
	for (auto const &line_result: range) {
		if (line_result.has_value()) {
			NodeRef root = line_result->root();
			(void)root.kind();
		} else {
			(void)(!line_result.error().message.empty());
		}
	}
	return 0;
}
