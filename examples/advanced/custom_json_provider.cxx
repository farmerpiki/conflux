// Advanced JSON provider example: direct-write encoding and custom decoding.
import conflux.json.boundary;
import conflux.net.http.native_json;
import std;

namespace jb = conflux::json::boundary;
namespace hj = conflux::http::json;

struct Metric {
	int value{};
};

struct CompactMetricProvider {
	static std::expected<void, jb::Error> write_json(
		Metric const &metric,
		jb::DumpOptions const &,
		auto &&sink) {
		sink(std::string_view{R"({"v":)"});
		auto encoded = std::to_string(metric.value);
		sink(std::string_view{encoded});
		sink(std::string_view{"}"});
		return {};
	}

	template<class T>
		requires std::same_as<T, Metric>
	static std::expected<T, jb::Error> decode_json(
		std::string_view input,
		jb::DecodeOptions const &) {
		if (!input.starts_with(R"({"v":)") || !input.ends_with('}')) {
			return std::unexpected(
				jb::Error{
					.stage = jb::ErrorStage::decode,
					.code = jb::ErrorCode::invalid_value,
					.message = "expected compact metric object"});
		}

		auto value_text = input.substr(5, input.size() - 6);
		int value{};
		auto const *first = value_text.data();
		auto const *last = value_text.data() + value_text.size();
		auto [ptr, ec] = std::from_chars(first, last, value);
		if (ec != std::errc{} || ptr != last) {
			return std::unexpected(
				jb::Error{
					.stage = jb::ErrorStage::decode,
					.code = jb::ErrorCode::invalid_value,
					.message = "expected integer metric value"});
		}
		return T{.value = value};
	}
};

int main() {
	auto response = hj::try_response_with<CompactMetricProvider>(Metric{.value = 42});
	if (!response) {
		std::println(std::cerr, "encode failed: {}", response.error().message);
		return 1;
	}

	auto decoded = jb::decode_with<CompactMetricProvider, Metric>(response->text_body());
	if (!decoded) {
		std::println(std::cerr, "decode failed: {}", decoded.error().message);
		return 1;
	}

	std::println("{} -> {}", response->text_body(), decoded->value);
	return 0;
}
