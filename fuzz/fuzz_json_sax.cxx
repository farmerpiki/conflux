import std;
import conflux.types;
import conflux.json;

using namespace std;
using namespace conflux::json;
struct FuzzHandler {
	SZ depth{};
	expected<void, JsonError> on_begin_object() {
		++depth;
		return {};
	}
	expected<void, JsonError> on_end_object() {
		--depth;
		return {};
	}
	expected<void, JsonError> on_begin_array() {
		++depth;
		return {};
	}
	expected<void, JsonError> on_end_array() {
		--depth;
		return {};
	}
	expected<void, JsonError> on_key(
		SV) {
		return {};
	}
	expected<void, JsonError> on_string(
		SV) {
		return {};
	}
	expected<void, JsonError> on_i64(
		i64) {
		return {};
	}
	expected<void, JsonError> on_u64(
		u64) {
		return {};
	}
	expected<void, JsonError> on_double(
		double) {
		return {};
	}
	expected<void, JsonError> on_bool(
		bool) {
		return {};
	}
	expected<void, JsonError> on_null() { return {}; }
};
extern "C" int LLVMFuzzerTestOneInput(
	u8 const *data,
	SZ size) {
	if (size == 0) {
		return 0;
	}

	SV input{reinterpret_cast<char const *>(data), size};

	JsonParseOptions opts{.max_depth = LimitOption::bound(256)};
	FuzzHandler handler;
	(void)parse_sax(input, handler, opts);
	return 0;
}
