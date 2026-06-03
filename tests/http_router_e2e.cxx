#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.router;

TEST_CASE(
	"middleware next is one-shot",
	"[middleware]") {
	conflux::http::Router router;
	router.use([](conflux::http::OwnedRequest const &req, conflux::http::Router::Handler const &next) {
		auto _ = next(req);
		return next(req);
	});
	router.get("/twice", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/twice";
	CHECK_THROWS_AS(router.dispatch(req), std::logic_error);
}

TEST_CASE(
	"router dispatch preserves generic route priority before literal index hits") {
	conflux::http::Router router;
	router.get("/{id}", [](conflux::http::RequestView const &req) {
		return conflux::http::Response::text(std::string{"generic:"} + std::string{req.params["id"]});
	});
	router.get("/health", [](conflux::http::RequestView const &) { return conflux::http::Response::text("literal"); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/health";
	req.version = "HTTP/1.1";

	CHECK(router.dispatch(req).text_body() == "generic:health");
}

TEST_CASE(
	"router dispatch uses method-scoped literal lookup") {
	conflux::http::Router router;
	router.post("/health", [](conflux::http::RequestView const &) { return conflux::http::Response::text("post"); });
	router.get("/health", [](conflux::http::RequestView const &) { return conflux::http::Response::text("get"); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/health";
	req.version = "HTTP/1.1";

	CHECK(router.dispatch(req).text_body() == "get");
}
