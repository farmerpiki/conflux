#include <catch2/catch_test_macros.hpp>

import std;
import conflux.net.router;
import conflux.work;

TEST_CASE(
	"router: wildcard {*path} captures entire tail") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["path"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/files/docs/readme.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "docs/readme.txt");
}

TEST_CASE(
	"router: wildcard {*path} captures empty tail when path ends at prefix") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["path"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/files/";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body().empty());
}

TEST_CASE(
	"router: wildcard with prefix param captures both") {
	conflux::http::Router router;
	router.get("/{version}/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::format("{}/{}", req.params["version"], req.params["path"]));
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/v2/files/a/b/c.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "v2/a/b/c.txt");
}

TEST_CASE(
	"router: non-matching path returns 404") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["path"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/other/stuff";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 404);
}

TEST_CASE(
	"router: percent-encoded path param is URL-decoded") {
	conflux::http::Router router;
	router.get("/hello/{name}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["name"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/hello/hello%20world";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "hello world");
}

TEST_CASE(
	"router: malformed route pattern is rejected") {
	conflux::http::Router router;
	CHECK_THROWS_AS(
		router.get(
			"/admin/{tenant}/{action",
			[](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("bad"); }),
		std::invalid_argument);

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/admin/acme";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 404);
}

TEST_CASE(
	"router: wildcard route_info preserves star notation") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::text("ok");
	});
	auto infos = router.route_infos();
	REQUIRE(infos.size() == 1);
	CHECK(infos[0].path_pattern == "/files/{*path}");
	REQUIRE(infos[0].path_params.size() == 1);
	CHECK(infos[0].path_params[0] == "path");
}

TEST_CASE(
	"router: wildcard tail with percent-encoded segment is URL-decoded") {
	conflux::http::Router router;
	router.get("/files/{*path}", [](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::string{req.params["path"]});
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/files/dir/my%20file.txt";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "dir/my file.txt");
}

TEST_CASE(
	"router: on_not_found custom handler called for unmatched path") {
	conflux::http::Router router;
	router.get("/exists", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("ok"); });
	router.on_not_found([](conflux::http::OwnedRequest const &req) {
		return conflux::http::Response::text(std::format("nope:{}", req.path));
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/missing";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "nope:/missing");
}

TEST_CASE(
	"router: on_error custom handler called when route throws") {
	conflux::http::Router router;
	router.get("/boom", [](conflux::http::OwnedRequest const &) -> conflux::http::Response {
		throw std::runtime_error{"oops"};
	});
	std::string captured_what;
	router.on_error([&](conflux::http::OwnedRequest const &, std::exception const &ex) {
		captured_what = ex.what();
		return conflux::http::Response::text("caught");
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/boom";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 200);
	REQUIRE(resp.text_body() == "caught");
	REQUIRE(captured_what == "oops");
}

TEST_CASE(
	"router: default 404 when no on_not_found is set") {
	conflux::http::Router router;
	router.get("/a", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("a"); });
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/missing";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 404);
}

TEST_CASE(
	"router: default 500 when route throws and no on_error is set") {
	conflux::http::Router router;
	router.get("/boom", [](conflux::http::OwnedRequest const &) -> conflux::http::Response {
		throw std::runtime_error{"crash"};
	});
	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/boom";
	auto resp = router.dispatch(req);
	REQUIRE(resp.status == 500);
}

TEST_CASE(
	"router: explicit HEAD route wins before GET fallback") {
	conflux::http::Router router;
	router.add("HEAD", "/secret", [](conflux::http::OwnedRequest const &) {
		return conflux::http::Response::forbidden("head denied");
	});
	router.get("/secret", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("get ok"); });

	conflux::http::OwnedRequest req;
	req.method = "HEAD";
	req.path = "/secret";
	auto resp = router.dispatch(req);

	CHECK(resp.status == conflux::http::kHttpForbidden);
	CHECK(resp.head_only);
}

TEST_CASE(
	"router: explicit HEAD context route wins before GET fallback") {
	conflux::http::Router router;
	router.add_context(
		"HEAD",
		"/secret",
		[](conflux::http::RequestView const &,
		   conflux::http::RequestContext const &) -> conflux::work::Task<conflux::http::Response> {
			co_return conflux::http::Response::forbidden("head denied");
		});
	router.get_context(
		"/secret",
		[](conflux::http::RequestView const &, conflux::http::RequestContext const &)
			-> conflux::work::Task<conflux::http::Response> { co_return conflux::http::Response::text("get ok"); });

	conflux::http::OwnedRequest req;
	req.method = "HEAD";
	req.path = "/secret";
	auto resp = router.dispatch_context(req, conflux::http::RequestContext{});
	REQUIRE(resp.has_value());
	REQUIRE(resp->is_deferred());
	auto deferred = resp->deferred_response_ptr();
	REQUIRE(deferred->is_ready());
	auto completed = deferred->take_ready();
	REQUIRE(completed.has_value());

	CHECK(completed->status == conflux::http::kHttpForbidden);
	CHECK(completed->head_only);
}

TEST_CASE(
	"router: group middleware protects context routes") {
	conflux::http::Router router;
	router.group("/admin", [](auto &group) {
		group.use([](conflux::http::RequestView const &, auto const &) {
			return conflux::http::Response::unauthorized("Bearer");
		});
		group.get_context(
			"/context",
			[](conflux::http::RequestView const &, conflux::http::RequestContext const &)
				-> conflux::work::Task<conflux::http::Response> { co_return conflux::http::Response::text("secret"); });
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/admin/context";
	auto resp = router.dispatch_context(req, conflux::http::RequestContext{});
	REQUIRE(resp.has_value());
	REQUIRE(resp->is_deferred());
	auto deferred = resp->deferred_response_ptr();
	REQUIRE(deferred->is_ready());
	auto completed = deferred->take_ready();
	REQUIRE(completed.has_value());
	CHECK(completed->status == 401);
}
