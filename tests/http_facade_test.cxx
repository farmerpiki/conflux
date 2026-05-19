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
	REQUIRE(app.state<std::shared_ptr<std::string>>().value != nullptr);
	CHECK(*app.state<std::shared_ptr<std::string>>().get() == "owned");
}

TEST_CASE(
	"http facade: stream helper writes buffered response bodies",
	"[http.facade]") {
	auto response = http::stream(
		[](http::StreamSink &out) {
			out.write("alpha");
			out.write(":");
			out.write("beta");
		},
		"text/plain; charset=utf-8");

	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "text/plain; charset=utf-8");
	CHECK(response.text_body() == "alpha:beta");
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
	"http facade: validate reports invalid route patterns",
	"[http.facade]") {
	auto app = http::app();
	app.get("/files/{*path}/tail", [] { return http::text("bad"); });
	app.get("/users/{id", [] { return http::text("bad"); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 2);
	CHECK(report.issues[0].message == "invalid route pattern: wildcard parameter must be the final segment");
	CHECK(report.issues[0].path == "/files/{*path}/tail");
	CHECK(report.issues[0].source_file.ends_with("http_facade_test.cxx"));
	CHECK(report.issues[0].source_line > 0);
	CHECK(report.issues[1].message == "invalid route pattern: unmatched path parameter braces");
	CHECK(report.issues[1].path == "/users/{id");
}

TEST_CASE(
	"http facade: validate reports ambiguous same-shape routes",
	"[http.facade]") {
	auto app = http::app();
	app.get("/users/{id}", [] { return http::text("one"); });
	app.get("/users/{slug}", [] { return http::text("two"); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "ambiguous route; also matches /users/{id}");
	CHECK(report.issues[0].method == "GET");
	CHECK(report.issues[0].path == "/users/{slug}");
	CHECK(report.issues[0].related_source_file.ends_with("http_facade_test.cxx"));
	CHECK(report.issues[0].related_source_line > 0);
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
	"http facade: validate reports missing static roots",
	"[http.facade]") {
	auto app = http::app();
	app.serve_static("/assets", "/tmp/conflux-missing-static-root-for-test");

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].method == "STATIC");
	CHECK(report.issues[0].path == "/assets");
	CHECK(report.issues[0].message.find("static root does not exist") != std::string::npos);
	CHECK(report.issues[0].source_file.ends_with("http_facade_test.cxx"));
	CHECK(report.issues[0].source_line > 0);
}

TEST_CASE(
	"http facade: validate reports invalid TLS config",
	"[http.facade]") {
	auto cfg = http::Config::public_server();
	cfg.cert_file = "/tmp/cert.pem";
	cfg.http3.enabled = true;
	cfg.http_redirect_to_https = true;
	cfg.virtual_hosts.push_back(VirtualHost{.hostname = "api.example.test", .cert_file = "/tmp/api-cert.pem"});
	cfg.virtual_hosts.push_back(VirtualHost{.key_file = "/tmp/empty-host-key.pem"});
	auto app = http::app(std::move(cfg));

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	auto has_issue = [&](std::string_view message) {
		return std::ranges::any_of(report.issues, [&](auto const &issue) { return issue.message == message; });
	};
	CHECK(has_issue("TLS config invalid: cert_file and key_file must be set together"));
	CHECK(has_issue("TLS config invalid: HTTP/3 requires cert_file and key_file"));
	CHECK(has_issue("TLS config invalid: HTTPS redirect requires cert_file and key_file"));
	CHECK(has_issue("TLS config invalid: virtual host 'api.example.test' cert_file and key_file must be set together"));
	CHECK(has_issue("TLS config invalid: virtual host 'api.example.test' requires primary cert_file and key_file"));
	CHECK(has_issue("TLS config invalid: virtual host hostname is empty"));
	for (auto const &issue: report.issues) {
		CHECK(issue.method == "APP");
		CHECK(issue.path == "config");
	}
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
	"http facade: validate reports duplicate app state registration",
	"[http.facade]") {
	auto app = http::app();
	std::string first = "first";
	std::string second = "second";
	app.state(first);
	app.state(second);

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].method == "APP");
	CHECK(report.issues[0].path == "state");
	CHECK(report.issues[0].message.find("duplicate app state") != std::string::npos);
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
	"http facade: route builder decorates metadata",
	"[http.facade]") {
	auto app = http::app();
	app.route("POST", "/upload", [](http::BodyText) { return http::no_content(); })
		.name("upload.create")
		.max_body_size(1024 * 1024)
		.timeout(std::chrono::seconds{5})
		.rate_limit("uploads")
		.auth_policy("user")
		.openapi_summary("Upload a small body");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].method == "POST");
	CHECK(routes[0].path == "/upload");
	CHECK(routes[0].name == "upload.create");
	CHECK(routes[0].max_body_size == 1024 * 1024);
	CHECK(routes[0].timeout == std::chrono::seconds{5});
	CHECK(routes[0].rate_limit == "uploads");
	CHECK(routes[0].auth_policy == "user");
	CHECK(routes[0].openapi_summary == "Upload a small body");
	CHECK(app.route_table() == "POST /upload [app] name=upload.create BodyText");
}

TEST_CASE(
	"http facade: verb helpers return route metadata handles",
	"[http.facade]") {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check").openapi_summary("Health check");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].method == "GET");
	CHECK(routes[0].name == "health.check");
	CHECK(routes[0].openapi_summary == "Health check");
	CHECK(app.route_table() == "GET /health [app] name=health.check");
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
	CHECK(spec.find(R"("title":"Facade API")") != std::string::npos);
	CHECK(spec.find(R"("version":"0.2.0")") != std::string::npos);
	CHECK(spec.find(R"("/users/{id}")") != std::string::npos);
	CHECK(spec.find(R"("get")") != std::string::npos);
	CHECK(spec.find(R"("operationId":"users.show")") != std::string::npos);
	CHECK(spec.find(R"("summary":"Show a user")") != std::string::npos);
	CHECK(spec.find(R"("name":"id")") != std::string::npos);
}

TEST_CASE(
	"http facade: app openapi spec includes JSON request bodies",
	"[http.facade]") {
	auto app = http::app();
	app.post_body<FacadeAnswer>("/answers", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	auto spec = app.openapi_spec();
	CHECK(spec.find(R"("requestBody")") != std::string::npos);
	CHECK(spec.find(R"("application/json")") != std::string::npos);
	CHECK(spec.find(R"("application/problem+json")") != std::string::npos);
}

TEST_CASE(
	"http facade: app openapi spec groups methods by path",
	"[http.facade]") {
	auto app = http::app();
	app.route("GET", "/items", [] { return http::no_content(); }).name("items.list");
	app.route("POST", "/items", [](http::BodyText) { return http::no_content(); }).name("items.create");

	auto spec = app.openapi_spec();
	auto const path_pos = spec.find(R"("/items")");
	REQUIRE(path_pos != std::string::npos);
	CHECK(spec.find(R"("get")", path_pos) != std::string::npos);
	CHECK(spec.find(R"("post")", path_pos) != std::string::npos);
	CHECK(spec.find(R"("operationId":"items.list")", path_pos) != std::string::npos);
	CHECK(spec.find(R"("operationId":"items.create")", path_pos) != std::string::npos);
	CHECK(spec.find(R"("/items")", path_pos + 1) == std::string::npos);
}

TEST_CASE(
	"http facade: app openapi spec includes auth policies",
	"[http.facade]") {
	auto app = http::app();
	app.get("/private", [] { return http::no_content(); }).auth_policy("user");

	auto spec = app.openapi_spec();
	CHECK(
		spec.find("\"securitySchemes\":{\"bearerAuth\":{\"type\":\"http\",\"scheme\":\"bearer\"}}")
		!= std::string::npos);
	CHECK(spec.find("\"security\":[{\"bearerAuth\":[]}]") != std::string::npos);
	CHECK(spec.find("\"x-auth-policy\":\"user\"") != std::string::npos);
}

TEST_CASE(
	"http facade: app openapi handler serves metadata spec",
	"[http.facade]") {
	auto app = http::app();
	app.get("/health", [] { return http::text("ok"); }).name("health.check");
	app.get("/openapi.json", app.openapi_handler("Facade API", "1.2.3"));

	HttpRequest req;
	req.method = "GET";
	req.path = "/openapi.json";

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "application/json");
	CHECK(response.text_body().find(R"("title":"Facade API")") != std::string_view::npos);
	CHECK(response.text_body().find(R"("operationId":"health.check")") != std::string_view::npos);
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
	"http facade: listen validates before creating server",
	"[http.facade]") {
	auto app = http::app();
	app.get("/needs-state", [](http::State<std::string> state) { return http::text(state.get()); });

	auto server = std::move(app).listen();
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
	"http facade: typed route parameter tags dispatch with untyped extractor names",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/typed/{id:u64}">(
		[](http::Path<"id", std::uint64_t> id) { return http::text(std::format("{}", id.get())); });

	CHECK(app.validate().ok());

	HttpRequest req;
	req.method = "GET";
	req.path = "/typed/42";

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "42");
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

	HttpRequest req;
	req.method = "GET";
	req.path = "/teams/core/users/42";

	auto response = app.router().dispatch(req);
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
	"http facade: app handlers can receive request id extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/request-id", [](http::RequestId request_id) { return http::text(request_id.get()); });

	HttpRequest req;
	req.method = "GET";
	req.path = "/request-id";
	req.headers["x-request-id"] = "req-123";

	auto response = app.router().dispatch(req);
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

	HttpRequest req;
	req.method = "GET";
	req.path = "/conn";
	req.remote_addr = "203.0.113.10";
	req.is_tls = true;

	auto response = app.router().dispatch(req);
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

	HttpRequest req;
	req.method = "GET";
	req.path = "/trace";
	req.headers["traceparent"] = "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01";

	auto response = app.router().dispatch(req);
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
	app.get("/bearer", [](http::Bearer bearer) { return http::text(bearer.get()); });

	HttpRequest req;
	req.method = "GET";
	req.path = "/bearer";
	req.headers["authorization"] = "bearer  token-123 \t";

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "token-123");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "Bearer");
}

TEST_CASE(
	"http facade: app handlers can receive basic auth extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/basic", [](http::BasicAuth auth) {
		return http::text(std::format("{}:{}", auth.username, auth.password));
	});

	HttpRequest req;
	req.method = "GET";
	req.path = "/basic";
	req.headers["authorization"] = "Basic YWxpY2U6czNjcmV0";

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "alice:s3cret");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "BasicAuth");
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

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].consumes == std::vector<std::string>{"application/json", "application/problem+json"});
	CHECK(routes[0].produces == std::vector<std::string>{"application/json"});
}

TEST_CASE(
	"http facade: JSON body routes enforce route-local body limits",
	"[http.facade]") {
	auto app = http::app();
	app.post_body<FacadeAnswer>("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; })
		.max_body_size(8);

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].max_body_size == 8);

	HttpRequest req;
	req.method = "POST";
	req.path = "/json";
	req.headers["content-type"] = "application/json";
	req.body = R"({"value":"too large"})";

	auto too_large = app.router().dispatch(req);
	CHECK(too_large.status == kHttpRequestEntityTooLarge);
	CHECK(too_large.status_text == "Content Too Large");
}

TEST_CASE(
	"http facade: JSON app options provide default decode options and body limit",
	"[http.facade]") {
	auto app = http::app();
	app.json_options(
		http::AppJsonOptions{
			.decode = {.unknown_members = conflux::json::boundary::UnknownMemberPolicy::ignore},
			.max_body_size = 64});
	app.post_body<FacadeAnswer>("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	HttpRequest req;
	req.method = "POST";
	req.path = "/json";
	req.headers["content-type"] = "application/json";
	req.body = R"({"value":"ok","ignored":true})";

	auto ok = app.router().dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == R"({"value":"ok"})");

	req.body = R"({"value":"this body is deliberately longer than the configured route default limit"})";
	auto too_large = app.router().dispatch(req);
	CHECK(too_large.status == kHttpRequestEntityTooLarge);
}

TEST_CASE(
	"http facade: app handlers can receive body extractors",
	"[http.facade]") {
	auto app = http::app();
	app.post("/echo-text", [](http::BodyText body) { return http::text(body.get()); });
	app.post("/echo-bytes", [](http::BodyBytes body) { return http::text(body.get()); });
	app.post("/echo-owned", [](http::OwnedBodyBytes body) { return http::text(body.get()); });

	HttpRequest req;
	req.method = "POST";
	req.path = "/echo-text";
	req.body = "hello";
	CHECK(app.router().dispatch(req).text_body() == "hello");

	req.path = "/echo-bytes";
	req.body = "bytes";
	CHECK(app.router().dispatch(req).text_body() == "bytes");

	req.path = "/echo-owned";
	req.body = "owned";
	CHECK(app.router().dispatch(req).text_body() == "owned");
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

	HttpRequest req;
	req.method = "POST";
	req.path = "/upload";
	req.form["title"] = "Report";
	req.files.push_back(UploadedFile::borrowed("upload", "report.txt", "text/plain", "file-body"));

	auto response = app.router().dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "Report:report.txt:file-body");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	REQUIRE(routes[0].extractors.size() == 1);
	CHECK(routes[0].extractors[0] == "Multipart");
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
	"http facade: file helper reads small files",
	"[http.facade]") {
	auto path = std::filesystem::temp_directory_path() / "conflux_http_facade_file_helper.txt";
	{
		std::ofstream out{path, std::ios::binary};
		out << "file-body";
	}

	auto response = http::file(path, "text/plain");
	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "text/plain");
	CHECK(response.text_body() == "file-body");

	std::filesystem::remove(path);
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
