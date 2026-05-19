#include <catch2/catch_test_macros.hpp>
#include <conflux/http.hpp>

import conflux.http;
import std;

namespace http = conflux::http;

struct MixedPayload {
	std::string value;
};

template<>
struct JsonMembers<MixedPayload> {
	static constexpr auto members() {
		return std::tuple{
			json_member("value", &MixedPayload::value),
		};
	}
	static constexpr std::string_view type_name() { return "MixedPayload"; }
};

TEST_CASE(
	"http facade: mixed include and import smoke",
	"[http.facade]") {
	auto app = http::app();
	app.get("/", [] { return http::json(MixedPayload{.value = "ok"}); });
	CHECK(app.routes().size() == 1);
}
