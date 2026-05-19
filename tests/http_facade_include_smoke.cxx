#include <catch2/catch_test_macros.hpp>
#include <conflux/http.hpp>

import std;

namespace http = conflux::http;

static_assert(requires { typename http::RequestView; });
static_assert(requires { typename http::Request; });
static_assert(requires { typename http::Response; });

TEST_CASE(
	"http facade: public include smoke",
	"[http.facade]") {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); });

	HttpRequest req;
	req.method = "GET";
	req.path = "/health";

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "ok");
}
