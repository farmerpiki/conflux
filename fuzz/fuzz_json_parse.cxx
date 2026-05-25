// libFuzzer driver for conflux::json::parse.
// Invariants:
//   - never crash on any byte sequence
//   - parse ok  -> dump -> parse2 -> is_value_equal(root1, root2)
//   - parse fail -> message non-empty

import std;
import conflux.types;
import conflux.json;

#include "json_roundtrip_fuzz_helpers.hxx"

using namespace std;
using namespace conflux::json;
extern "C" int LLVMFuzzerTestOneInput(
	std::uint8_t const *data,
	std::size_t size) {
	JsonParseOptions opts;
	opts.max_depth = LimitOption::bound(256); // prevent stack overflow on deep nesting
	return conflux::fuzz::json_parse_dump_roundtrip(data, size, opts);
}
