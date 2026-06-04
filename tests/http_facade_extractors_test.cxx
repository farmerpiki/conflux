#include <catch2/catch_test_macros.hpp>

import std;
import conflux.http;
import conflux.json;
import conflux.json.boundary;
import conflux.work;

namespace http = conflux::http;
using namespace conflux::http;
using namespace conflux::json;

struct FacadeAnswer {
	std::string value;
};

struct FacadeSearch {
	std::string q;
	std::uint32_t page{};
};

struct FacadeOptionalSearch {
	std::string q;
	std::optional<std::uint32_t> page;
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

template<>
struct conflux::json::JsonMembers<FacadeSearch> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("q", &FacadeSearch::q),
			conflux::json::json_member("page", &FacadeSearch::page),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeSearch"; }
};

template<>
struct conflux::json::JsonMembers<FacadeOptionalSearch> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("q", &FacadeOptionalSearch::q),
			conflux::json::json_member("page", &FacadeOptionalSearch::page),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeOptionalSearch"; }
};

conflux::json::Document require_json_text(
	std::string text) {
	INFO(text);
	auto doc = conflux::json::parse_copy(text);
	REQUIRE(doc.has_value());
	return std::move(*doc);
}

conflux::json::Document require_json_body(
	conflux::http::Response const &response,
	int status,
	std::string_view content_type = "application/problem+json") {
	REQUIRE(response.status == status);
	REQUIRE(response.content_type == content_type);
	return require_json_text(std::string{response.text_body()});
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

TEST_CASE(
	"http facade: app handlers can return Json wrappers",
	"[http.facade]") {
	auto app = http::app();
	app.get("/answer", [] { return http::Json{FacadeAnswer{.value = "ok"}}; });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/answer";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "application/json");
	CHECK(response.text_body() == R"({"value":"ok"})");
}

TEST_CASE(
	"http facade: app handlers can receive State extractors",
	"[http.facade]") {
	auto app = http::app();
	std::string value = "state-value";
	app.state(value);

	app.get("/state", [](http::State<std::string> state) { return http::text(state.get()); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/state";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "state-value");
}

TEST_CASE(
	"http facade: app handlers can receive Path extractors",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/hello/{name}">([](http::Path<"name"> name) { return http::text(name.get()); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/hello/Ada";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "Ada");
}

TEST_CASE(
	"http facade: compile-time route spelling records normal metadata",
	"[http.facade]") {
	auto app = http::app();
	app.post<"/compile-time/{id}">(
		[](http::Path<"id", std::uint64_t> id) { return http::text(std::format("{}", id.get())); });

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].method == "POST");
	CHECK(routes[0].path == "/compile-time/{id}");
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "Path<id>");
	CHECK(
		routes[0].path_param_types
		== std::vector<std::pair<std::string, std::string>>{
			{"id", ""}
    });
	CHECK(app.validate().ok());
}

TEST_CASE(
	"http facade: typed route parameter tags dispatch with untyped extractor names",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/typed/{id:u64}">(
		[](http::Path<"id", std::uint64_t> id) { return http::text(std::format("{}", id.get())); });

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].path_params == std::vector<std::string>{"id"});
	CHECK(
		routes[0].path_param_types
		== std::vector<std::pair<std::string, std::string>>{
			{"id", "u64"}
    });
	CHECK(app.validate().ok());

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/typed/42";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "42");
}

TEST_CASE(
	"http facade: fixed typed routes pass path params as plain handler arguments",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/todos/{id:i64}">([](std::int64_t id) { return http::text(std::format("todo={}", id)); });

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].path == "/todos/{id:i64}");
	CHECK(routes[0].extractors == std::vector<std::string>{"Path<id>"});
	CHECK(
		routes[0].path_param_types
		== std::vector<std::pair<std::string, std::string>>{
			{"id", "i64"}
    });
	CHECK(app.validate().ok());

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/todos/-42";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "todo=-42");

	req.path = "/todos/nope";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "Path");
	check_json_string_at(bad_doc, "/name", "id");
}

TEST_CASE(
	"http facade: fixed typed routes support task handlers",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/async-todos/{id:i64}">([](std::int64_t id) -> conflux::work::Task<http::Response> {
		co_return http::text(std::format("todo={}", id));
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].path == "/async-todos/{id:i64}");
	CHECK(routes[0].extractors == std::vector<std::string>{"Path<id>"});
	CHECK(
		routes[0].path_param_types
		== std::vector<std::pair<std::string, std::string>>{
			{"id", "i64"}
    });
	CHECK(http::router(app).has_context_routes());
	CHECK(app.validate().ok());
}

TEST_CASE(
	"http facade: positional path extractors dispatch by capture order",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/teams/{team}/users/{id:u64}">([](http::PathAt<0> team, http::PathAt<1, std::uint64_t> id) {
		return http::text(std::format("{}:{}", team.get(), id.get()));
	});

	CHECK(app.validate().ok());

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 2);
	CHECK(routes[0].extractors[0] == "PathAt<0>");
	CHECK(routes[0].extractors[1] == "PathAt<1>");

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/teams/core/users/42";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "core:42");
}

TEST_CASE(
	"http facade: validate reports positional path parameter mismatch",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/typed/{id:u64}">(
		[](http::PathAt<0, std::int64_t> id) { return http::text(std::format("{}", id.get())); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "path parameter type mismatch for Path<0>: route has u64, handler expects i64");
}

TEST_CASE(
	"http facade: validate reports typed route parameter mismatch",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/typed/{id:u64}">(
		[](http::Path<"id", std::int64_t> id) { return http::text(std::format("{}", id.get())); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "path parameter type mismatch for Path<id>: route has u64, handler expects i64");
}

TEST_CASE(
	"http facade: app handlers can receive field extractors",
	"[http.facade]") {
	auto app = http::app();
	app.get(
		"/fields",
		[](http::Query<"q"> q, http::Header<"x-request-id"> request_id, http::Cookie<"session"> session) {
			return http::text(std::format("{}:{}:{}", q.get(), request_id.get(), session.get()));
		});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/fields";
	req.query["q"] = "search";
	req.headers["x-request-id"] = "req-1";
	req.cookies["session"] = "cookie-1";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "search:req-1:cookie-1");
}

TEST_CASE(
	"http facade: app handlers can receive request id extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/request-id", [](http::RequestId request_id) { return http::text(request_id.get()); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/request-id";
	req.headers["x-request-id"] = "req-123";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "req-123");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "RequestId");
}

TEST_CASE(
	"http facade: app handlers can receive connection info extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/conn", [](http::ConnectionInfo conn) {
		return http::text(std::format("{}:{}", conn.remote_addr, conn.is_tls ? "tls" : "plain"));
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/conn";
	req.remote_addr = "203.0.113.10";
	req.is_tls = true;

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "203.0.113.10:tls");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "ConnectionInfo");
}

TEST_CASE(
	"http facade: app handlers can receive trace context extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/trace", [](http::TraceContext trace) { return http::text(trace.traceparent); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/trace";
	req.headers["traceparent"] = "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "TraceContext");
}

TEST_CASE(
	"http facade: app handlers can receive bearer auth extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/bearer", [](http::BearerToken bearer) { return http::text(bearer.get()); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/bearer";
	req.headers["authorization"] = "bearer  token-123 \t";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "token-123");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "BearerToken");
}

TEST_CASE(
	"http facade: required bearer extractor rejects missing authorization",
	"[http.facade]") {
	auto app = http::app();
	app.get("/bearer", [](http::RequiredBearerToken bearer) { return http::text(bearer.get()); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/bearer";

	auto missing = http::router(app).dispatch(req);
	CHECK(missing.status == kHttpUnauthorized);
	CHECK(missing.headers["WWW-Authenticate"] == "Bearer");

	req.headers["authorization"] = "Bearer secret";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == "secret");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].bearer_token_policy == "bearer");
	CHECK(routes[0].extractors == std::vector<std::string>{"RequiredBearerToken"});
	auto const spec = app.openapi_spec("Auth", "1.0");
	CHECK(
		spec.find("\"securitySchemes\":{\"bearerAuth\":{\"type\":\"http\",\"scheme\":\"bearer\"}}")
		!= std::string::npos);
	CHECK(spec.find("\"security\":[{\"bearerAuth\":[]}]") != std::string::npos);
}

TEST_CASE(
	"http facade: optional bearer extractor does not reject missing authorization",
	"[http.facade]") {
	auto app = http::app();
	app.get("/bearer", [](http::OptionalBearerToken bearer) { return http::text(bearer.get().value_or("none")); });

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/bearer";
	CHECK(http::router(app).dispatch(req).text_body() == "none");

	req.headers["authorization"] = "Bearer secret";
	CHECK(http::router(app).dispatch(req).text_body() == "secret");
}

TEST_CASE(
	"http facade: app handlers can receive basic auth extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/basic", [](http::BasicAuth auth) {
		return http::text(std::format("{}:{}", auth.username, auth.password));
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/basic";
	req.headers["authorization"] = "Basic YWxpY2U6czNjcmV0";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "alice:s3cret");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "BasicAuth");
}

TEST_CASE(
	"http facade: required basic auth extractor rejects missing credentials",
	"[http.facade]") {
	auto app = http::app();
	app.get("/basic", [](http::RequiredBasicAuth auth) {
		return http::text(std::format("{}:{}", auth->username, auth->password));
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/basic";

	auto missing = http::router(app).dispatch(req);
	CHECK(missing.status == kHttpUnauthorized);
	CHECK(missing.headers["WWW-Authenticate"] == "Basic");

	req.headers["authorization"] = "Basic YWxpY2U6czNjcmV0";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == "alice:s3cret");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"RequiredBasicAuth"});
	auto doc = require_json_text(app.openapi_spec("Auth", "1.0"));
	check_json_string_at(doc, "/components/securitySchemes/basicAuth/type", "http");
	check_json_string_at(doc, "/components/securitySchemes/basicAuth/scheme", "basic");
	REQUIRE(require_json_pointer(doc, "/paths/~1basic/get/security/0/basicAuth").as_array().has_value());
}

TEST_CASE(
	"http facade: optional basic auth extractor does not reject missing credentials",
	"[http.facade]") {
	auto app = http::app();
	app.get("/basic", [](http::OptionalBasicAuth auth) {
		if (!auth.get()) {
			return http::text("none");
		}
		return http::text(std::format("{}:{}", auth.get()->username, auth.get()->password));
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/basic";
	CHECK(http::router(app).dispatch(req).text_body() == "none");

	req.headers["authorization"] = "Basic YWxpY2U6czNjcmV0";
	CHECK(http::router(app).dispatch(req).text_body() == "alice:s3cret");
}

TEST_CASE(
	"http facade: typed field extractors parse scalars and reject malformed values",
	"[http.facade]") {
	auto app = http::app();
	app.get("/items/{id}", [](http::Path<"id", std::uint64_t> id, http::Query<"page", std::uint32_t> page) {
		return http::text(std::format("{}:{}", id.get(), page.get()));
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/items/42";
	req.params["id"] = "42";
	req.query["page"] = "7";
	CHECK(http::router(app).dispatch(req).text_body() == "42:7");

	req.query["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "Query");
	check_json_string_at(bad_doc, "/name", "page");
	check_json_string_at(bad_doc, "/kind", "invalid");
#ifndef NDEBUG
	CHECK(bad.text_body().find(R"("target":)") != std::string_view::npos);
#endif
}

TEST_CASE(
	"http facade: typed field extractors support optional scalar values",
	"[http.facade]") {
	auto app = http::app();
	app.get(
		"/items/{id}",
		[](http::Path<"id", std::optional<std::uint64_t>> id,
		   http::Query<"page", std::optional<std::uint32_t>> page,
		   http::Header<"x-limit", std::optional<std::uint32_t>> limit,
		   http::Cookie<"session", std::optional<std::uint32_t>> session) {
			return http::text(
				std::format("{}:{}:{}:{}", id.value_or(0), page.value_or(1), limit.value_or(10), session.value_or(99)));
		});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/items/42";
	req.params["id"] = "42";
	req.query["page"] = "7";
	req.headers["x-limit"] = "3";
	req.cookies["session"] = "5";
	CHECK(http::router(app).dispatch(req).text_body() == "42:7:3:5");

	req.query.clear();
	req.headers.clear();
	req.cookies.clear();
	CHECK(http::router(app).dispatch(req).text_body() == "42:1:10:99");

	req.query["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "Query");
	check_json_string_at(bad_doc, "/name", "page");
	check_json_string_at(bad_doc, "/kind", "invalid");
}

TEST_CASE(
	"http facade: required and optional field extractor aliases share typed parsing",
	"[http.facade]") {
	auto app = http::app();
	app.get(
		"/aliases",
		[](http::RequiredQuery<"id", std::uint64_t> id,
		   http::OptionalHeader<"x-page", std::uint32_t> page,
		   http::OptionalCookie<"session", std::uint32_t> session) {
			return http::text(std::format("{}:{}:{}", id.get(), page.value_or(1), session.value_or(0)));
		});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/aliases";
	req.query["id"] = "42";
	req.headers["x-page"] = "7";
	CHECK(http::router(app).dispatch(req).text_body() == "42:7:0");

	req.headers.clear();
	req.cookies["session"] = "5";
	CHECK(http::router(app).dispatch(req).text_body() == "42:1:5");

	req.query.clear();
	auto missing = http::router(app).dispatch(req);
	auto missing_doc = require_json_body(missing, kHttpBadRequest);
	check_json_string_at(missing_doc, "/extractor", "Query");
	check_json_string_at(missing_doc, "/kind", "missing");
}

TEST_CASE(
	"http facade: required string-like field extractors reject absence but accept empty values",
	"[http.facade]") {
	auto app = http::app();
	app.post(
		"/required-fields",
		[](http::RequiredQuery<"q"> q,
		   http::RequiredHeader<"x-token"> token,
		   http::RequiredCookie<"session"> session,
		   http::RequiredForm<"name"> name) {
			return http::text(std::format("{}:{}:{}:{}", q.get(), token.get(), session.get(), name.get()));
		});

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/required-fields";
	req.query["q"] = "";
	req.headers["x-token"] = "";
	req.cookies["session"] = "";
	req.form["name"] = "";
	CHECK(http::router(app).dispatch(req).text_body() == ":::");

	req.query.clear();
	auto missing_query = http::router(app).dispatch(req);
	auto missing_query_doc = require_json_body(missing_query, kHttpBadRequest);
	check_json_string_at(missing_query_doc, "/extractor", "Query");
	check_json_string_at(missing_query_doc, "/name", "q");
	check_json_string_at(missing_query_doc, "/kind", "missing");

	req.query["q"] = "";
	req.headers.clear();
	auto missing_header = http::router(app).dispatch(req);
	auto missing_header_doc = require_json_body(missing_header, kHttpBadRequest);
	check_json_string_at(missing_header_doc, "/extractor", "Header");
	check_json_string_at(missing_header_doc, "/name", "x-token");
	check_json_string_at(missing_header_doc, "/kind", "missing");

	req.headers["x-token"] = "";
	req.cookies.clear();
	auto missing_cookie = http::router(app).dispatch(req);
	auto missing_cookie_doc = require_json_body(missing_cookie, kHttpBadRequest);
	check_json_string_at(missing_cookie_doc, "/extractor", "Cookie");
	check_json_string_at(missing_cookie_doc, "/name", "session");
	check_json_string_at(missing_cookie_doc, "/kind", "missing");

	req.cookies["session"] = "";
	req.form.clear();
	auto missing_form = http::router(app).dispatch(req);
	auto missing_form_doc = require_json_body(missing_form, kHttpBadRequest);
	check_json_string_at(missing_form_doc, "/extractor", "Form");
	check_json_string_at(missing_form_doc, "/name", "name");
	check_json_string_at(missing_form_doc, "/kind", "missing");
}

TEST_CASE(
	"http facade: app handlers can receive form extractors",
	"[http.facade]") {
	auto app = http::app();
	app.post("/submit", [](http::Form<"name"> name, http::Form<"age", std::uint32_t> age) {
		return http::text(std::format("{}:{}", name.get(), age.get()));
	});

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/submit";
	req.form["name"] = "Ada";
	req.form["age"] = "37";
	CHECK(http::router(app).dispatch(req).text_body() == "Ada:37");

	req.form["age"] = "bad";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "Form");
	check_json_string_at(bad_doc, "/name", "age");
	check_json_string_at(bad_doc, "/kind", "invalid");
}

TEST_CASE(
	"http facade: form extractors support optional scalar values",
	"[http.facade]") {
	auto app = http::app();
	app.post("/submit", [](http::Form<"age", std::optional<std::uint32_t>> age) {
		return http::text(std::format("{}", age.value_or(0)));
	});

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/submit";
	req.form["age"] = "37";
	CHECK(http::router(app).dispatch(req).text_body() == "37");

	req.form.clear();
	CHECK(http::router(app).dispatch(req).text_body() == "0");

	req.form["age"] = "bad";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "Form");
	check_json_string_at(bad_doc, "/name", "age");
	check_json_string_at(bad_doc, "/kind", "invalid");
}

TEST_CASE(
	"http facade: required and optional form extractor aliases share typed parsing",
	"[http.facade]") {
	auto app = http::app();
	app.post(
		"/form-aliases",
		[](http::RequiredForm<"id", std::uint64_t> id, http::OptionalForm<"age", std::uint32_t> age) {
			return http::text(std::format("{}:{}", id.get(), age.value_or(0)));
		});

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/form-aliases";
	req.form["id"] = "42";
	req.form["age"] = "37";
	CHECK(http::router(app).dispatch(req).text_body() == "42:37");

	req.form.erase("age");
	CHECK(http::router(app).dispatch(req).text_body() == "42:0");

	req.form.clear();
	auto missing = http::router(app).dispatch(req);
	auto missing_doc = require_json_body(missing, kHttpBadRequest);
	check_json_string_at(missing_doc, "/extractor", "Form");
	check_json_string_at(missing_doc, "/kind", "missing");
}

TEST_CASE(
	"http facade: app handlers can receive aggregate query params",
	"[http.facade]") {
	auto app = http::app();
	app.get("/search", [](http::QueryParams<FacadeSearch> search) {
		return http::text(std::format("{}:{}", search->q, search->page));
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/search";
	req.query["q"] = "conflux";
	req.query["page"] = "3";
	CHECK(http::router(app).dispatch(req).text_body() == "conflux:3");

	req.query["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "QueryParams");
	check_json_string_at(bad_doc, "/name", "page");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"QueryParams"});
}

TEST_CASE(
	"http facade: aggregate query params support optional fields",
	"[http.facade]") {
	auto app = http::app();
	app.get("/search", [](http::QueryParams<FacadeOptionalSearch> search) {
		return http::text(std::format("{}:{}", search->q, search->page.value_or(1)));
	});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/search";
	req.query["q"] = "conflux";
	CHECK(http::router(app).dispatch(req).text_body() == "conflux:1");

	req.query["page"] = "5";
	CHECK(http::router(app).dispatch(req).text_body() == "conflux:5");

	req.query["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "QueryParams");
	check_json_string_at(bad_doc, "/name", "page");
	check_json_string_at(bad_doc, "/kind", "invalid");
}

TEST_CASE(
	"http facade: app handlers can receive aggregate form params",
	"[http.facade]") {
	auto app = http::app();
	app.post("/search", [](http::FormParams<FacadeSearch> search) {
		return http::text(std::format("{}:{}", search->q, search->page));
	});

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/search";
	req.form["q"] = "conflux";
	req.form["page"] = "4";
	CHECK(http::router(app).dispatch(req).text_body() == "conflux:4");

	req.form["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "FormParams");
	check_json_string_at(bad_doc, "/name", "page");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"FormParams"});
}

TEST_CASE(
	"http facade: aggregate form params support optional fields",
	"[http.facade]") {
	auto app = http::app();
	app.post("/search", [](http::FormParams<FacadeOptionalSearch> search) {
		return http::text(std::format("{}:{}", search->q, search->page.value_or(1)));
	});

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/search";
	req.form["q"] = "conflux";
	CHECK(http::router(app).dispatch(req).text_body() == "conflux:1");

	req.form["page"] = "5";
	CHECK(http::router(app).dispatch(req).text_body() == "conflux:5");

	req.form["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	auto bad_doc = require_json_body(bad, kHttpBadRequest);
	check_json_string_at(bad_doc, "/extractor", "FormParams");
	check_json_string_at(bad_doc, "/name", "page");
	check_json_string_at(bad_doc, "/kind", "invalid");
}

TEST_CASE(
	"http facade: JSON body routes validate content type",
	"[http.facade]") {
	auto app = http::app();
	app.post("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/json";
	req.body = R"({"value":"ok"})";

	auto missing = http::router(app).dispatch(req);
	auto missing_doc = require_json_body(missing, kHttpBadRequest);
	check_json_string_at(missing_doc, "/code", "unsupported_content_type");
	check_json_string_at(missing_doc, "/expected", "application/json");

	req.headers["content-type"] = "application/json";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == R"({"value":"ok"})");

	req.headers["content-type"] = "application/json; charset=utf-8";
	auto with_params = http::router(app).dispatch(req);
	CHECK(with_params.status == kHttpOk);

	req.headers["content-type"] = "application/jsonp";
	auto jsonp = http::router(app).dispatch(req);
	CHECK(jsonp.status == kHttpBadRequest);
	CHECK(jsonp.content_type == "application/problem+json");

	req.headers["content-type"] = "application/json";
	req.body = R"({"value":)";
	auto malformed = http::router(app).dispatch(req);
	auto malformed_doc = require_json_body(malformed, kHttpBadRequest);
	check_json_string_at(malformed_doc, "/code", "json.decode.type_mismatch");
	check_json_string_at(malformed_doc, "/stage", "parse");
	check_json_string_at(malformed_doc, "/kind", "unexpected_eof");
	CHECK(malformed.text_body().find(R"("source":)") != std::string_view::npos);

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].consumes == std::vector<std::string>{"application/json", "application/problem+json"});
	CHECK(routes[0].produces == std::vector<std::string>{"application/json"});
}

TEST_CASE(
	"http facade: JSON document extractor parses request body",
	"[http.facade]") {
	auto app = http::app();
	app.post("/json-doc", [](http::JsonDocument doc) {
		auto dumped = doc->dump();
		REQUIRE(dumped.has_value());
		return http::text(*dumped);
	});

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/json-doc";
	req.body = R"({"value":"ok"})";

	auto missing = http::router(app).dispatch(req);
	auto missing_doc = require_json_body(missing, kHttpBadRequest);
	check_json_string_at(missing_doc, "/code", "unsupported_content_type");
	check_json_string_at(missing_doc, "/expected", "application/json");

	req.headers["content-type"] = "application/json";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == R"({"value":"ok"})");

	req.headers["content-type"] = "application/problem+json; charset=utf-8";
	auto problem_with_params = http::router(app).dispatch(req);
	CHECK(problem_with_params.status == kHttpOk);

	req.headers["content-type"] = "application/jsonp";
	auto jsonp = http::router(app).dispatch(req);
	CHECK(jsonp.status == kHttpBadRequest);
	CHECK(jsonp.content_type == "application/problem+json");

	req.headers["content-type"] = "application/json";
	req.body = R"({"value":)";
	auto malformed = http::router(app).dispatch(req);
	auto malformed_doc = require_json_body(malformed, kHttpBadRequest);
	check_json_string_at(malformed_doc, "/code", "json.decode.type_mismatch");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"JsonDocument"});
	CHECK(routes[0].consumes == std::vector<std::string>{"application/json", "application/problem+json"});
}

TEST_CASE(
	"http facade: JSON document extractor honors app body limit",
	"[http.facade]") {
	auto app = http::app();
	app.json_options(http::AppJsonOptions{.max_body_size = 8});
	app.post("/json-doc", [](http::JsonDocument) { return http::no_content(); });

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/json-doc";
	req.headers["content-type"] = "application/json";
	req.body = R"({"value":"too large"})";

	auto too_large = http::router(app).dispatch(req);
	auto too_large_doc = require_json_body(too_large, kHttpRequestEntityTooLarge);
	check_json_string_at(too_large_doc, "/code", "body_too_large");
}

TEST_CASE(
	"http facade: JsonPatch extractor validates content type and patch shape",
	"[http.facade]") {
	auto app = http::app();
	app.patch("/patch", [](http::JsonPatch patch) {
		auto ok = conflux::json::validate_patch(patch.value.root());
		REQUIRE(ok.has_value());
		return http::no_content();
	});

	conflux::http::OwnedRequest req;
	req.method = "PATCH";
	req.path = "/patch";
	req.body = R"([{"op":"add","path":"/name","value":"Ada"}])";

	auto wrong = http::router(app).dispatch(req);
	auto wrong_doc = require_json_body(wrong, kHttpBadRequest);
	check_json_string_at(wrong_doc, "/code", "unsupported_content_type");

	req.headers["content-type"] = "application/json-patch+json";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpNoContent);

	req.headers["content-type"] = "application/json-patch+json; charset=utf-8";
	auto with_params = http::router(app).dispatch(req);
	CHECK(with_params.status == kHttpNoContent);

	req.headers["content-type"] = "application/json-patch+jsonp";
	auto jsonp = http::router(app).dispatch(req);
	CHECK(jsonp.status == kHttpBadRequest);

	req.headers["content-type"] = "application/json-patch+json";
	req.body = R"([{"path":"/name","value":"Ada"}])";
	auto invalid = http::router(app).dispatch(req);
	auto invalid_doc = require_json_body(invalid, kHttpBadRequest);
	check_json_string_at(invalid_doc, "/code", "patch_op_missing");
	check_json_string_at(invalid_doc, "/stage", "json_patch");

	auto spec = require_json_text(app.openapi_spec("Patch API", "1.0"));
	REQUIRE(require_json_pointer(spec, "/paths/~1patch/patch/requestBody/content/application~1json-patch+json/schema")
				.as_object()
				.has_value());
	auto required = require_json_pointer(
		spec,
		"/paths/~1patch/patch/requestBody/content/application~1json-patch+json/schema/items/required");
	REQUIRE(required.as_array().has_value());
	check_json_string_at(
		spec,
		"/paths/~1patch/patch/requestBody/content/application~1json-patch+json/schema/items/required/0",
		"op");
	check_json_string_at(
		spec,
		"/paths/~1patch/patch/requestBody/content/application~1json-patch+json/schema/items/required/1",
		"path");
}

TEST_CASE(
	"http facade: MergePatch extractor validates content type and body limit",
	"[http.facade]") {
	auto app = http::app();
	app.patch(
		   "/merge",
		   [](http::MergePatch patch) {
			   auto dumped = patch->dump();
			   REQUIRE(dumped.has_value());
			   return http::text(*dumped);
		   })
		.max_body_size(8);

	conflux::http::OwnedRequest req;
	req.method = "PATCH";
	req.path = "/merge";
	req.body = R"({"a":1})";

	auto wrong = http::router(app).dispatch(req);
	CHECK(wrong.status == kHttpBadRequest);
	CHECK(wrong.content_type == "application/problem+json");

	req.headers["content-type"] = "application/merge-patch+json";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == R"({"a":1})");

	req.headers["content-type"] = "application/merge-patch+json; charset=utf-8";
	auto with_params = http::router(app).dispatch(req);
	CHECK(with_params.status == kHttpOk);

	req.headers["content-type"] = "application/merge-patch+jsonp";
	auto jsonp = http::router(app).dispatch(req);
	CHECK(jsonp.status == kHttpBadRequest);

	req.headers["content-type"] = "application/merge-patch+json";
	req.body = R"({"long":true})";
	auto too_large = http::router(app).dispatch(req);
	CHECK(too_large.status == kHttpRequestEntityTooLarge);
	CHECK(too_large.content_type == "application/problem+json");

	auto spec = require_json_text(app.openapi_spec("Merge API", "1.0"));
	REQUIRE(require_json_pointer(spec, "/paths/~1merge/patch/requestBody/content/application~1merge-patch+json/schema")
				.as_object()
				.has_value());
}

TEST_CASE(
	"http facade: JSON body routes enforce route-local body limits",
	"[http.facade]") {
	auto app = http::app();
	app.post("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; }).max_body_size(8);

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].max_body_size == 8);

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/json";
	req.headers["content-type"] = "application/json";
	req.body = R"({"value":"too large"})";

	auto too_large = http::router(app).dispatch(req);
	CHECK(too_large.status == kHttpRequestEntityTooLarge);
	CHECK(too_large.status_text == "Content Too Large");
	auto too_large_doc = require_json_body(too_large, kHttpRequestEntityTooLarge);
	check_json_string_at(too_large_doc, "/code", "body_too_large");
}

TEST_CASE(
	"http facade: JSON app options provide default decode options and body limit",
	"[http.facade]") {
	auto app = http::app();
	app.json_options(
		http::AppJsonOptions{
			.decode = {.unknown_members = conflux::json::boundary::UnknownMemberPolicy::ignore},
			.dump = {.pretty = true},
			.max_body_size = 64});
	app.post("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/json";
	req.headers["content-type"] = "application/json";
	req.body = R"({"value":"ok","ignored":true})";

	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body().find('\n') != std::string_view::npos);
	CHECK(ok.text_body().find(R"("value": "ok")") != std::string_view::npos);

	req.body = R"({"value":"this body is deliberately longer than the configured route default limit"})";
	auto too_large = http::router(app).dispatch(req);
	CHECK(too_large.status == kHttpRequestEntityTooLarge);
}

TEST_CASE(
	"http facade: app handlers can receive body extractors",
	"[http.facade]") {
	auto app = http::app();
	app.post("/echo-text", [](http::BodyText body) { return http::text(body.get()); });
	app.post("/echo-bytes", [](http::BodyBytes body) { return http::text(body.text_view()); });
	app.post("/echo-owned", [](http::OwnedBodyBytes body) { return http::text(body.get()); });

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/echo-text";
	req.body = "hello";
	CHECK(http::router(app).dispatch(req).text_body() == "hello");

	req.path = "/echo-bytes";
	req.body = "bytes";
	CHECK(http::router(app).dispatch(req).text_body() == "bytes");

	req.path = "/echo-owned";
	req.body = "owned";
	CHECK(http::router(app).dispatch(req).text_body() == "owned");
}

TEST_CASE(
	"http facade: app handlers can receive multipart extractor",
	"[http.facade]") {
	auto app = http::app();
	app.post("/upload", [](http::Multipart multipart) {
		auto const *file = multipart.file("upload");
		return http::text(
			std::format(
				"{}:{}:{}",
				multipart.form_value("title"),
				file == nullptr ? std::string_view{} : file->filename,
				file == nullptr ? std::string_view{} : file->data));
	});

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/upload";
	req.form["title"] = "Report";
	req.files.push_back(conflux::http::UploadedFile::borrowed("upload", "report.txt", "text/plain", "file-body"));

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "Report:report.txt:file-body");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "Multipart");
}
