#include <catch2/catch_test_macros.hpp>

import std;
import conflux.http;
import conflux.http.extended;
import conflux.json;
import conflux.json.boundary;
import conflux.net.config;

namespace http = conflux::http;
using namespace conflux::http;
using namespace conflux::json;
using conflux::http::ConfigIssueCode;
using conflux::http::VirtualHost;

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
	"http facade: validate reports duplicate routes",
	"[http.facade]") {
	auto app = http::app();
	app.get("/same", [] { return http::text("one"); });
	app.get("/same", [] { return http::text("two"); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].code == "http.route.duplicate");
	CHECK(report.issues[0].message == "duplicate route");
	CHECK(report.issues[0].method == "GET");
	CHECK(report.issues[0].path == "/same");
	CHECK(report.issues[0].source_file.ends_with("http_facade_validation_test.cxx"));
	CHECK(report.issues[0].source_line > 0);
	CHECK(report.issues[0].related_source_file.ends_with("http_facade_validation_test.cxx"));
	CHECK(report.issues[0].related_source_line > 0);
	auto detailed = report.detailed_summary();
	CHECK(detailed.find("GET /same [http.route.duplicate]: duplicate route at ") != std::string::npos);
	CHECK(detailed.find(" related ") != std::string::npos);
}

TEST_CASE(
	"http facade: validate aggregates config issues",
	"[http.facade]") {
	auto cfg = http::Config::development();
	cfg.parser_limits.max_headers = 0;
	auto app = http::App{cfg};

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE_FALSE(report.config_issues.empty());
	CHECK(report.config_issues.front().code == ConfigIssueCode::invalid_value);
	CHECK(report.summary().find("config.invalid_value") != std::string::npos);
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
	CHECK(report.summary() == "GET /needs-state [app.state.missing]: missing app state");
}

TEST_CASE(
	"http facade: validate reports mismatched path extractor",
	"[http.facade]") {
	auto app = http::app();
	app.get("/users/{id}", [](http::Path<"slug"> slug) { return http::text(slug.get()); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "missing path parameter for Path<slug>. Available path parameters: id.");
	CHECK(
		report.summary()
		== "GET /users/{id} [app.validation]: missing path parameter for Path<slug>. Available path parameters: id.");
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
	CHECK(report.issues[0].source_file.ends_with("http_facade_validation_test.cxx"));
	CHECK(report.issues[0].source_line > 0);
	CHECK(report.issues[1].message == "invalid route pattern: unmatched path parameter braces");
	CHECK(report.issues[1].path == "/users/{id");
}

TEST_CASE(
	"http facade: validate reports unknown typed route suffix",
	"[http.facade]") {
	auto app = http::app();
	app.get("/users/{id:uuid}", [] { return http::text("bad"); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "invalid route pattern: unknown path parameter type 'uuid'");
	CHECK(report.issues[0].path == "/users/{id:uuid}");
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
	CHECK(report.issues[0].code == "http.route.ambiguous");
	CHECK(report.issues[0].message == "ambiguous route; also matches /users/{id}");
	CHECK(report.issues[0].method == "GET");
	CHECK(report.issues[0].path == "/users/{slug}");
	CHECK(report.issues[0].related_source_file.ends_with("http_facade_validation_test.cxx"));
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
	CHECK(report.summary() == "GET /body [app.validation]: body extractor used on GET route");
}

TEST_CASE(
	"http facade: GET body extractors require explicit opt-in",
	"[http.facade]") {
	auto app = http::app();
	app.get("/body", [](http::BodyText body) { return http::text(body.get()); }).allow_get_body();

	auto report = app.validate();
	CHECK(report.ok());

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].allow_get_body);
}

TEST_CASE(
	"http facade: validate reports body extractors without body limits",
	"[http.facade]") {
	auto cfg = http::Config::public_server();
	cfg.max_body_size = 0;
	auto app = http::app(std::move(cfg));
	app.post("/body", [](http::BodyText body) { return http::text(body.get()); });

	auto report = app.validate();
	REQUIRE_FALSE(report.ok());
	REQUIRE(report.issues.size() == 1);
	CHECK(report.issues[0].message == "body extractor used without body limit");
	CHECK(report.summary() == "POST /body [app.validation]: body extractor used without body limit");
}

TEST_CASE(
	"http facade: validate accepts route-local body limits",
	"[http.facade]") {
	auto cfg = http::Config::public_server();
	cfg.max_body_size = 0;
	auto app = http::app(std::move(cfg));
	app.post("/body", [](http::BodyText body) { return http::text(body.get()); }).max_body_size(1024);

	CHECK(app.validate().ok());
}

TEST_CASE(
	"http facade: JSON extractors honor configured app body limit",
	"[http.facade]") {
	auto cfg = http::Config::public_server();
	cfg.max_body_size = 8;
	auto app = http::app(std::move(cfg));
	app.post("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	REQUIRE(app.validate().ok());

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/json";
	req.headers["content-type"] = "application/json";
	req.body = R"({"value":"too large"})";

	auto too_large = http::router(app).dispatch(req);
	auto too_large_doc = require_json_body(too_large, kHttpRequestEntityTooLarge);
	check_json_string_at(too_large_doc, "/code", "body_too_large");
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
	CHECK(report.issues[0].source_file.ends_with("http_facade_validation_test.cxx"));
	CHECK(report.issues[0].source_line > 0);

	auto mounts = app.static_mounts();
	REQUIRE(mounts.size() == 1);
	CHECK(mounts[0].url_prefix == "/assets");
	CHECK(mounts[0].root_dir == "/tmp/conflux-missing-static-root-for-test");
	CHECK(mounts[0].source_file.ends_with("http_facade_validation_test.cxx"));
	CHECK(mounts[0].source_line > 0);
	CHECK(app.route_table() == "STATIC /assets root=/tmp/conflux-missing-static-root-for-test");
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
