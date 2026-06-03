#include <catch2/catch_test_macros.hpp>

import std;
import conflux.json;
import conflux.net.auth;
import conflux.net.config;
import conflux.net.openapi;
import conflux.net.router;
import conflux.tests.support;

using namespace conflux::tests;

namespace {

conflux::json::Document parse_openapi_spec(
	std::string spec) {
	auto doc = conflux::json::parse_copy(std::move(spec));
	REQUIRE(doc.has_value());
	return std::move(*doc);
}

conflux::json::NodeRef require_json_pointer(
	conflux::json::Document const &doc,
	std::string_view pointer) {
	auto node = doc.root().at_pointer(pointer);
	REQUIRE(node.has_value());
	return *node;
}

void check_json_string_at(
	conflux::json::Document const &doc,
	std::string_view pointer,
	std::string_view expected) {
	auto node = require_json_pointer(doc, pointer);
	auto value = node.as_string();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}

std::string extract_body(
	std::string_view resp) {
	auto pos = resp.find("\r\n\r\n");
	if (pos == std::string_view::npos) {
		return {};
	}
	return std::string{resp.substr(pos + 4)};
}

} // namespace

TEST_CASE(
	"openapi: spec contains openapi 3.0.0 root key") {
	conflux::http::Router router;
	router.get("/hello/{name}", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	router.post("/items", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, "Test API", "0.1.0"));
	check_json_string_at(doc, "/openapi", "3.0.0");
}

TEST_CASE(
	"openapi: spec includes registered path with path parameter") {
	conflux::http::Router router;
	router.get("/hello/{name}", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, "Test API", "0.1.0"));
	REQUIRE(require_json_pointer(doc, "/paths/~1hello~1{name}/get").as_object().has_value());
	check_json_string_at(doc, "/paths/~1hello~1{name}/get/parameters/0/name", "name");
	check_json_string_at(doc, "/paths/~1hello~1{name}/get/parameters/0/in", "path");
}

TEST_CASE(
	"openapi: spec includes title and version from arguments") {
	conflux::http::Router router;
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, "My Service", "2.3.4"));
	check_json_string_at(doc, "/info/title", "My Service");
	check_json_string_at(doc, "/info/version", "2.3.4");
}

TEST_CASE(
	"openapi: spec includes method in lowercase") {
	conflux::http::Router router;
	router.post("/items", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router));
	REQUIRE(require_json_pointer(doc, "/paths/~1items/post").as_object().has_value());
}

TEST_CASE(
	"openapi: title with special characters is properly JSON-escaped") {
	conflux::http::Router router;
	router.get("/", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text(""); });
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, R"(My "API" & More)"));
	check_json_string_at(doc, "/info/title", R"(My "API" & More)");
}

TEST_CASE(
	"openapi: empty router produces valid paths object") {
	conflux::http::Router router;
	auto doc = parse_openapi_spec(conflux::http::openapi_spec(router, "Empty", "0.0.1"));
	auto paths = require_json_pointer(doc, "/paths").as_object();
	REQUIRE(paths.has_value());
	CHECK(paths->size() == 0);
	check_json_string_at(doc, "/info/title", "Empty");
}

TEST_CASE(
	"openapi_handler_protected: wrong bearer token returns 401") {
	conflux::http::Router router;
	router.get("/ping", [](conflux::http::OwnedRequest const &) { return conflux::http::Response::text("pong"); });
	std::vector<conflux::http::Router::Middleware> chain;
	chain.push_back(conflux::http::bearer_auth_middleware([](std::string_view token) { return token == "apikey"; }));
	router.get("/openapi.json", conflux::http::openapi_handler_protected(router, "API", "1.0.0", std::move(chain)));

	conflux::http::Config const cfg{.port = 0, .rings = 1};
	std::uint16_t port = test_servers().start(cfg, std::move(router));

	auto resp_no_auth = http_get_on(port, "/openapi.json");
	REQUIRE(resp_no_auth.starts_with("HTTP/1.1 401"));

	auto resp_ok = http_get_on(port, "/openapi.json", "Authorization: Bearer apikey\r\n");
	REQUIRE(resp_ok.starts_with("HTTP/1.1 200"));
	REQUIRE(resp_ok.find("application/json") != std::string::npos);
	auto doc = parse_openapi_spec(extract_body(resp_ok));
	check_json_string_at(doc, "/openapi", "3.0.0");
}
