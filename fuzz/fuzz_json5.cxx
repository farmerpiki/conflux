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

	JsonParseOptions opts{.max_depth = LimitOption::bound(256), .mode = ParseMode::json5};

	auto res = parse(input, opts);
	if (!res) {
		auto const &err = res.error();
		if (err.message.empty()) {
			__builtin_trap();
		}
		return 0;
	}

	NodeRef const root = res->root();
	if (auto arr = root.as_array()) {
		for (NodeRef const e: arr->elements()) {
			auto _ = e.kind();
		}
	} else if (auto obj = root.as_object()) {
		for (auto const &[k, v]: obj->members()) {
			auto _ = k;
			auto _ = v.kind();
		}
	}

	auto dumped = res->dump();
	if (!dumped) {
		return 0;
	}
	auto res2 = parse(*dumped);
	if (!res2) {
		return 0;
	}
	if (!is_value_equal(root, res2->root())) {
		__builtin_trap();
	}
	return 0;
}
