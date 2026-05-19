#include <catch2/catch_test_macros.hpp>

import std;
import conflux.http;
import conflux.json;
import conflux.work;

namespace http = conflux::http;

static_assert(std::same_as<http::Task<http::Response>, conflux::work::Task<http::Response>>);
static_assert(std::same_as<http::Next, http::Router::Handler>);
static_assert(std::same_as<http::Config, Config>);

struct FacadeAnswer {
	std::string value;
};

template<>
struct JsonMembers<FacadeAnswer> {
	static constexpr auto members() {
		return std::tuple{
			json_member("value", &FacadeAnswer::value),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeAnswer"; }
};

TEST_CASE(
	"http facade: app state stores borrowed and shared typed state",
	"[http.facade]") {
	auto app = http::app();

	int borrowed = 42;
	app.state(borrowed);
	CHECK(app.state<int>().value == std::addressof(borrowed));
	CHECK(*app.state<int>() == 42);

	auto owned = std::make_shared<std::string>("owned");
	auto const *owned_ptr = owned.get();
	app.state(owned);
	CHECK(app.state<std::string>().value == owned_ptr);
	CHECK(*app.state<std::string>() == "owned");
}

TEST_CASE(
	"http facade: validate reports duplicate routes",
	"[http.facade]") {
	auto app = http::app();
	app.get("/same", [] { return http::text("one"); });
	app.get("/same", [] { return http::text("two"); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "duplicate route");
	CHECK(report.issues[0].method == "GET");
	CHECK(report.issues[0].path == "/same");
	CHECK(report.issues[0].source_file.ends_with("http_facade_test.cxx"));
	CHECK(report.issues[0].source_line > 0);
	CHECK(report.issues[0].related_source_file.ends_with("http_facade_test.cxx"));
	CHECK(report.issues[0].related_source_line > 0);
}

TEST_CASE(
	"http facade: validate reports missing app state",
	"[http.facade]") {
	auto app = http::app();
	app.get("/needs-state", [](http::State<std::string> state) { return http::text(state.get()); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "missing app state");
	CHECK(report.issues[0].method == "GET");
	CHECK(report.issues[0].path == "/needs-state");
	CHECK(report.summary() == "GET /needs-state: missing app state");
}

TEST_CASE(
	"http facade: validate reports mismatched path extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/users/{id}", [](http::Path<"slug"> slug) { return http::text(slug.get()); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "missing path parameter for Path<slug>");
	CHECK(report.summary() == "GET /users/{id}: missing path parameter for Path<slug>");
}

TEST_CASE(
	"http facade: validate reports body extractors on GET routes",
	"[http.facade]") {
	auto app = http::app();
	app.get("/body", [](http::BodyText body) { return http::text(body.get()); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "body extractor used on GET route");
	CHECK(report.summary() == "GET /body: body extractor used on GET route");
}

TEST_CASE(
	"http facade: validate accepts registered app state",
	"[http.facade]") {
	auto app = http::app();
	std::string value = "state-value";
	app.state(value);
	app.get("/needs-state", [](http::State<std::string> state) { return http::text(state.get()); });

	CHECK(app.validate().ok());
}

TEST_CASE(
	"http facade: routes expose app metadata",
	"[http.facade]") {
	auto app = http::app();
	std::string state = "state";
	app.state(state);
	app.get("/meta/{id}", [](http::Path<"id"> id, http::Query<"q"> q, http::State<std::string> store) {
		return http::text(std::format("{}:{}:{}", id.get(), q.get(), store.get()));
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].method == "GET");
	CHECK(routes[0].path == "/meta/{id}");
	CHECK(routes[0].handler_kind == "app");
	CHECK(routes[0].source_file.ends_with("http_facade_test.cxx"));
	CHECK(routes[0].source_line > 0);
	CHECK(routes[0].required_state_count == 1);
	REQUIRE(routes[0].extractors.size() == 3);
	CHECK(routes[0].extractors[0] == "Path<id>");
	CHECK(routes[0].extractors[1] == "Query<q>");
	CHECK(routes[0].extractors[2] == "State");
	CHECK(app.route_table() == "GET /meta/{id} [app] Path<id>,Query<q>,State");
}

TEST_CASE(
	"http facade: try_server rejects invalid app metadata",
	"[http.facade]") {
	auto app = http::app();
	app.get("/needs-state", [](http::State<std::string> state) { return http::text(state.get()); });

	auto server = std::move(app).try_server();
	REQUIRE_FALSE(server.has_value());
	CHECK(server.error() == "GET /needs-state: missing app state");
}

TEST_CASE(
	"http facade: app handlers can return Json wrappers",
	"[http.facade]") {
	auto app = http::app();
	app.get("/answer", [] { return http::Json{FacadeAnswer{.value = "ok"}}; });

	HttpRequest req;
	req.method = "GET";
	req.path = "/answer";

	auto response = app.router().dispatch(req);
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

	HttpRequest req;
	req.method = "GET";
	req.path = "/state";

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "state-value");
}

TEST_CASE(
	"http facade: app handlers can receive Path extractors",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/hello/{name}">([](http::Path<"name"> name) { return http::text(name.get()); });

	HttpRequest req;
	req.method = "GET";
	req.path = "/hello/Ada";

	auto response = app.router().dispatch(req);
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
	CHECK(app.validate().ok());
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

	HttpRequest req;
	req.method = "GET";
	req.path = "/fields";
	req.query["q"] = "search";
	req.headers["x-request-id"] = "req-1";
	req.cookies["session"] = "cookie-1";

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "search:req-1:cookie-1");
}

TEST_CASE(
	"http facade: typed field extractors parse scalars and reject malformed values",
	"[http.facade]") {
	auto app = http::app();
	app.get("/items/{id}", [](http::Path<"id", std::uint64_t> id, http::Query<"page", std::uint32_t> page) {
		return http::text(std::format("{}:{}", id.get(), page.get()));
	});

	HttpRequest req;
	req.method = "GET";
	req.path = "/items/42";
	req.params["id"] = "42";
	req.query["page"] = "7";
	CHECK(app.router().dispatch(req).text_body() == "42:7");

	req.query["page"] = "bad";
	auto bad = app.router().dispatch(req);
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.content_type == "application/json");
	CHECK(bad.text_body().find(R"("extractor":"Query")") != std::string_view::npos);
	CHECK(bad.text_body().find(R"("name":"page")") != std::string_view::npos);
	CHECK(bad.text_body().find(R"("kind":"invalid")") != std::string_view::npos);
}

TEST_CASE(
	"http facade: app handlers can receive form extractors",
	"[http.facade]") {
	auto app = http::app();
	app.post("/submit", [](http::Form<"name"> name, http::Form<"age", std::uint32_t> age) {
		return http::text(std::format("{}:{}", name.get(), age.get()));
	});

	HttpRequest req;
	req.method = "POST";
	req.path = "/submit";
	req.form["name"] = "Ada";
	req.form["age"] = "37";
	CHECK(app.router().dispatch(req).text_body() == "Ada:37");

	req.form["age"] = "bad";
	auto bad = app.router().dispatch(req);
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.content_type == "application/json");
	CHECK(bad.text_body().find(R"("extractor":"Form")") != std::string_view::npos);
	CHECK(bad.text_body().find(R"("name":"age")") != std::string_view::npos);
	CHECK(bad.text_body().find(R"("kind":"invalid")") != std::string_view::npos);
}

TEST_CASE(
	"http facade: JSON body routes validate content type",
	"[http.facade]") {
	auto app = http::app();
	app.post_body<FacadeAnswer>("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	HttpRequest req;
	req.method = "POST";
	req.path = "/json";
	req.body = R"({"value":"ok"})";

	auto missing = app.router().dispatch(req);
	CHECK(missing.status == kHttpBadRequest);
	CHECK(missing.text_body().find("unsupported content type") != std::string_view::npos);

	req.headers["content-type"] = "application/json";
	auto ok = app.router().dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == R"({"value":"ok"})");
}

TEST_CASE(
	"http facade: app handlers can receive body extractors",
	"[http.facade]") {
	auto app = http::app();
	app.post("/echo-text", [](http::BodyText body) { return http::text(body.get()); });
	app.post("/echo-bytes", [](http::BodyBytes body) { return http::text(body.get()); });

	HttpRequest req;
	req.method = "POST";
	req.path = "/echo-text";
	req.body = "hello";
	CHECK(app.router().dispatch(req).text_body() == "hello");

	req.path = "/echo-bytes";
	req.body = "bytes";
	CHECK(app.router().dispatch(req).text_body() == "bytes");
}

TEST_CASE(
	"http facade: response helpers cover redirect and created JSON",
	"[http.facade]") {
	auto redirect = http::redirect("/next");
	CHECK(redirect.status == kHttpFound);
	CHECK(redirect.headers["Location"] == "/next");

	auto created = http::into_response(http::created(FacadeAnswer{.value = "made"}).header("Location", "/answers/1"));
	CHECK(created.status == kHttpCreated);
	CHECK(created.content_type == "application/json");
	CHECK(created.headers["Location"] == "/answers/1");
	CHECK(created.text_body() == R"({"value":"made"})");
}

TEST_CASE(
	"http facade: problem helpers carry code/detail metadata",
	"[http.facade]") {
	auto problem = http::problem::bad_request("invalid_todo", "title is required");
	CHECK(problem.code == "invalid_todo");
	CHECK(problem.detail == "title is required");
	CHECK(problem.response.status == kHttpBadRequest);
	CHECK(problem.response.content_type == "application/json");
	CHECK(problem.response.text_body() == R"({"code":"invalid_todo","detail":"title is required"})");
}
