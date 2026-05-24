#include <catch2/catch_test_macros.hpp>

import conflux.http;
import conflux.net.http.server_types;
import std;

namespace http = conflux::http;

static_assert(requires { typename http::ClientRequest; });

TEST_CASE(
	"http facade: public import smoke",
	"[http.facade]") {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].method == "GET");
	CHECK(routes[0].path == "/health");
	CHECK(routes[0].name == "health.check");
}
