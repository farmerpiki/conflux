import std;
import conflux.types;
import conflux.json;

using namespace std;
using namespace conflux::json;
struct FuzzHandler {
	std::size_t depth{};
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
		std::string_view) {
		return {};
	}
	expected<void, JsonError> on_string(
		std::string_view) {
		return {};
	}
	expected<void, JsonError> on_i64(
		std::int64_t) {
		return {};
	}
	expected<void, JsonError> on_u64(
		std::uint64_t) {
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
	std::uint8_t const *data,
	std::size_t size) {
	if (size == 0) {
		return 0;
	}

	std::string_view input{reinterpret_cast<char const *>(data), size};

	JsonParseOptions opts{.max_depth = LimitOption::bound(256)};
	FuzzHandler handler;
	auto _ = parse_sax(input, handler, opts);
	return 0;
}
