#include <catch2/catch_test_macros.hpp>

import conflux.http;
import std;

namespace http = conflux::http;

TEST_CASE(
	"http facade: public import smoke",
	"[http.facade]") {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check");

	HttpRequest req;
	req.method = "GET";
	req.path = "/health";

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "ok");
	CHECK(app.routes()[0].name == "health.check");
}
