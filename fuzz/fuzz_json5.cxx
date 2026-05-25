import std;
import conflux.types;
import conflux.json;

#include "json_roundtrip_fuzz_helpers.hxx"

using namespace std;
using namespace conflux::json;
extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	JsonParseOptions opts{.max_depth = LimitOption::bound(256), .mode = ParseMode::json5};
	return conflux::fuzz::json_parse_dump_roundtrip(data, size, opts);
}
