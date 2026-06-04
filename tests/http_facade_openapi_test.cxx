#include <catch2/catch_test_macros.hpp>

import std;
import conflux.http;
import conflux.http.extended;
import conflux.json;
import conflux.json.boundary;

namespace http = conflux::http;
using namespace conflux::http;
using namespace conflux::json;

struct FacadeAnswer {
	std::string value;
};

template<>
struct conflux::json::JsonMembers<FacadeAnswer> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("value", &FacadeAnswer::value),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeAnswer"; }
};

conflux::json::Document require_json_text(
	std::string text) {
	INFO(text);
	auto doc = conflux::json::parse_copy(text);
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

void check_json_u64_at(
	conflux::json::Document const &doc,
	std::string_view pointer,
	std::uint64_t expected) {
	auto node = require_json_pointer(doc, pointer);
	auto value = node.as_u64();
	REQUIRE(value.has_value());
	CHECK(*value == expected);
}

TEST_CASE(
	"http facade: app openapi spec uses route metadata",
	"[http.facade]") {
	auto app = http::app();
	app.route(
		   "GET",
		   "/users/{id}",
		   [](http::Path<"id", std::uint64_t> id) { return http::text(std::format("{}", id.get())); })
		.name("users.show")
		.openapi_summary("Show a user");

	auto spec = app.openapi_spec("Facade API", "0.2.0");
	auto doc = require_json_text(std::move(spec));
	check_json_string_at(doc, "/info/title", "Facade API");
	check_json_string_at(doc, "/info/version", "0.2.0");
	check_json_string_at(doc, "/paths/~1users~1{id}/get/operationId", "users.show");
	check_json_string_at(doc, "/paths/~1users~1{id}/get/summary", "Show a user");
	check_json_string_at(doc, "/paths/~1users~1{id}/get/parameters/0/name", "id");
}

TEST_CASE(
	"http facade: app openapi spec maps typed path parameters",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/users/{id:u64}">(
		[](http::Path<"id", std::uint64_t> id) { return http::text(std::format("{}", id.get())); });
	app.get<"/teams/{slug}">([](http::Path<"slug"> slug) { return http::text(slug.get()); });

	auto spec = app.openapi_spec();
	auto doc = require_json_text(std::move(spec));
	check_json_string_at(doc, "/paths/~1users~1{id:u64}/get/parameters/0/name", "id");
	check_json_string_at(doc, "/paths/~1users~1{id:u64}/get/parameters/0/in", "path");
	check_json_string_at(doc, "/paths/~1users~1{id:u64}/get/parameters/0/schema/type", "integer");
	check_json_string_at(doc, "/paths/~1users~1{id:u64}/get/parameters/0/schema/format", "uint64");
	check_json_u64_at(doc, "/paths/~1users~1{id:u64}/get/parameters/0/schema/minimum", 0);
	check_json_string_at(doc, "/paths/~1teams~1{slug}/get/parameters/0/name", "slug");
	check_json_string_at(doc, "/paths/~1teams~1{slug}/get/parameters/0/schema/type", "string");
}

TEST_CASE(
	"http facade: app openapi spec includes JSON request bodies",
	"[http.facade]") {
	auto app = http::app();
	app.post("/answers", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	auto spec = app.openapi_spec();
	auto doc = require_json_text(std::move(spec));
	REQUIRE(require_json_pointer(doc, "/paths/~1answers/post/requestBody").as_object().has_value());
	check_json_string_at(
		doc,
		"/paths/~1answers/post/requestBody/content/application~1json/schema/properties/value/type",
		"string");
	check_json_string_at(doc, "/paths/~1answers/post/requestBody/content/application~1json/schema/required/0", "value");
	REQUIRE(require_json_pointer(doc, "/paths/~1answers/post/requestBody/content/application~1problem+json/schema")
				.as_object()
				.has_value());
}

TEST_CASE(
	"http facade: app route metadata records JSON responses",
	"[http.facade]") {
	auto app = http::app();
	app.get("/answer", [] { return http::Json{FacadeAnswer{.value = "ok"}}; });
	app.get("/expected", []() -> http::Result<http::Json<FacadeAnswer>> {
		return http::Json{FacadeAnswer{.value = "ok"}};
	});
	app.post("/created", [] { return http::created(FacadeAnswer{.value = "made"}); });

	auto routes = app.routes();
	REQUIRE(routes.size() == 3);
	CHECK(routes[0].produces == std::vector<std::string>{"application/json"});
	CHECK(routes[1].produces == std::vector<std::string>{"application/json"});
	CHECK(routes[2].success_status == kHttpCreated);
	CHECK(routes[2].produces == std::vector<std::string>{"application/json"});
	CHECK(routes[2].response_schema.find(R"("properties":{"value":{"type":"string"}})") != std::string::npos);
	CHECK_FALSE(routes[0].problem_response);
	CHECK(routes[1].problem_response);
	auto spec = app.openapi_spec();
	auto doc = require_json_text(std::move(spec));
	check_json_string_at(
		doc,
		"/paths/~1answer/get/responses/200/content/application~1json/schema/properties/value/type",
		"string");
	check_json_string_at(doc, "/paths/~1created/post/responses/201/description", "Created");
	check_json_string_at(
		doc,
		"/paths/~1created/post/responses/201/content/application~1json/schema/properties/value/type",
		"string");
	check_json_string_at(doc, "/paths/~1expected/get/responses/400/description", "Problem");
	REQUIRE(require_json_pointer(doc, "/paths/~1expected/get/responses/400/content/application~1problem+json/schema")
				.as_object()
				.has_value());
}

TEST_CASE(
	"http facade: app openapi spec groups methods by path",
	"[http.facade]") {
	auto app = http::app();
	app.route("GET", "/items", [] { return http::no_content(); }).name("items.list");
	app.route("POST", "/items", [](http::BodyText) { return http::no_content(); }).name("items.create");

	auto spec = app.openapi_spec();
	auto doc = require_json_text(std::move(spec));
	check_json_string_at(doc, "/paths/~1items/get/operationId", "items.list");
	check_json_string_at(doc, "/paths/~1items/post/operationId", "items.create");
}

TEST_CASE(
	"http facade: app openapi snapshot covers typed route policies",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/widgets/{id:u64}">([](http::Path<"id", std::uint64_t>) -> http::Result<http::Json<FacadeAnswer>> {
		   return http::Json{FacadeAnswer{.value = "ok"}};
	   })
		.name("widgets.show")
		.openapi_summary("Show widget")
		.require_bearer_token("user")
		.rate_limit("widgets")
		.timeout(std::chrono::seconds{5});

	CHECK(
		app.openapi_spec("Snapshot API", "1.0.0")
		== R"({"openapi":"3.0.0","info":{"title":"Snapshot API","version":"1.0.0"},"components":{"securitySchemes":{"bearerAuth":{"type":"http","scheme":"bearer"}}},"paths":{"/widgets/{id:u64}":{"get":{"operationId":"widgets.show","summary":"Show widget","security":[{"bearerAuth":[]}],"x-bearer-token-policy":"user","x-timeout-ms":5000,"x-rate-limit":"widgets","parameters":[{"name":"id","in":"path","required":true,"schema":{"type":"integer","format":"uint64","minimum":0}}],"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"type":"object","properties":{"value":{"type":"string"}},"required":["value"]}}}},"400":{"description":"Problem","content":{"application/problem+json":{"schema":{"type":"object"}}}},"401":{"description":"Unauthorized"},"429":{"description":"Too Many Requests"},"504":{"description":"Gateway Timeout"}}}}}})");
}

TEST_CASE(
	"http facade: app openapi spec includes auth policies",
	"[http.facade]") {
	auto app = http::app();
	app.get("/private", [] { return http::no_content(); }).require_bearer_token("user");

	auto spec = app.openapi_spec();
	CHECK(
		spec.find("\"securitySchemes\":{\"bearerAuth\":{\"type\":\"http\",\"scheme\":\"bearer\"}}")
		!= std::string::npos);
	auto doc = require_json_text(std::move(spec));
	REQUIRE(require_json_pointer(doc, "/paths/~1private/get/security/0/bearerAuth").as_array().has_value());
	check_json_string_at(doc, "/paths/~1private/get/x-bearer-token-policy", "user");
}

TEST_CASE(
	"http facade: app openapi spec includes route-local policy metadata",
	"[http.facade]") {
	auto app = http::app();
	app.use([](http::RequestView const &req, http::Next const &next) { return next(req); });
	app.post("/upload", [](http::BodyText) { return http::no_content(); })
		.timeout(std::chrono::seconds{5})
		.rate_limit("uploads")
		.require_bearer_token("user")
		.max_body_size(4096);

	auto spec = app.openapi_spec();
	auto doc = require_json_text(std::move(spec));
	check_json_u64_at(doc, "/paths/~1upload/post/x-timeout-ms", 5000);
	check_json_string_at(doc, "/paths/~1upload/post/x-rate-limit", "uploads");
	check_json_u64_at(doc, "/paths/~1upload/post/x-max-body-size", 4096);
	check_json_u64_at(doc, "/paths/~1upload/post/x-middleware-count", 1);
	check_json_string_at(doc, "/paths/~1upload/post/responses/401/description", "Unauthorized");
	check_json_string_at(doc, "/paths/~1upload/post/responses/429/description", "Too Many Requests");
	check_json_string_at(doc, "/paths/~1upload/post/responses/504/description", "Gateway Timeout");
}

TEST_CASE(
	"http facade: validate reports OpenAPI strict omissions",
	"[http.facade]") {
	auto app = http::app();
	app.openapi_strict();
	app.get("/plain", [] { return http::text("ok"); });
	app.post("/body", [](http::BodyText) { return http::no_content(); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	auto has_issue = [&](std::string_view path, std::string_view message) {
		return std::ranges::any_of(report.issues, [&](auto const &issue) {
			return issue.path == path && issue.message == message;
		});
	};
	CHECK(has_issue("/plain", "OpenAPI strict mode: route operationId is missing"));
	CHECK(has_issue("/plain", "OpenAPI strict mode: route summary is missing"));
	CHECK(has_issue("/plain", "OpenAPI strict mode: route response content metadata is missing"));
	CHECK(has_issue("/body", "OpenAPI strict mode: route request body content metadata is missing"));
}

TEST_CASE(
	"http facade: validate accepts OpenAPI strict metadata",
	"[http.facade]") {
	auto app = http::app();
	app.openapi_strict();
	app.get("/answer", [] { return http::Json{FacadeAnswer{.value = "ok"}}; })
		.name("answers.show")
		.openapi_summary("Show an answer");

	CHECK(app.validate().ok());
}

TEST_CASE(
	"http facade: app openapi handler serves metadata spec",
	"[http.facade]") {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check");
	app.get("/openapi.json", http::openapi_handler(app, "Facade API", "1.2.3"));

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/openapi.json";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "application/json");
	auto doc = require_json_text(std::string{response.text_body()});
	check_json_string_at(doc, "/info/title", "Facade API");
	check_json_string_at(doc, "/paths/~1health/get/operationId", "health.check");
}

TEST_CASE(
	"http facade: app openapi mounts metadata route",
	"[http.facade]") {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check");
	(void)app.openapi("/schema.json", "Mounted API", "2.0.0");

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/schema.json";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "application/json");
	auto doc = require_json_text(std::string{response.text_body()});
	check_json_string_at(doc, "/info/title", "Mounted API");
	check_json_string_at(doc, "/paths/~1health/get/operationId", "health.check");
}
