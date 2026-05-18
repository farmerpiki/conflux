import std;
import conflux.types;
import conflux.json;

using namespace std;
using namespace conflux::json;
extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	if (size == 0) {
		return 0;
	}

	std::string_view input{reinterpret_cast<char const *>(data), size};

	JsonParseOptions opts{.max_depth = LimitOption::bound(128)};
	NdjsonRange range{input, opts};
	for (auto const &line_result: range) {
		if (line_result.has_value()) {
			NodeRef root = line_result->root();
			auto _ = root.kind();
		} else {
			if (line_result.error().message.empty()) {
				__builtin_trap();
			}
		}
	}
	return 0;
}
