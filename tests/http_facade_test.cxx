#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

import std;
import conflux.types;
import conflux.http;
import conflux.http.extended;
import conflux.json;
import conflux.json.boundary;
import conflux.net.config;
import conflux.net.observability;
import conflux.net.router;
import conflux.work;

namespace http = conflux::http;

static_assert(std::same_as<http::Task<http::Response>, conflux::work::Task<http::Response>>);
static_assert(std::same_as<http::Config, Config>);
static_assert(std::same_as<http::DrainOptions, DrainOptions>);
static_assert(std::same_as<http::DrainReport, DrainReport>);
static_assert(std::same_as<http::DrainStreamPolicy, DrainStreamPolicy>);
static_assert(std::same_as<http::OverflowPolicy, OverflowPolicy>);
static_assert(std::same_as<http::PressureMetrics, HttpPressureMetrics>);
static_assert(std::same_as<decltype(std::declval<http::BodyBytes const &>().get()), std::span<std::byte const>>);
static_assert(std::same_as<decltype(std::declval<http::BodyBytes const &>().text_view()), std::string_view>);

TEST_CASE(
	"http facade: lifecycle and pressure vocabulary defaults are explicit",
	"[http.facade]") {
	http::DrainOptions drain{};
	CHECK(drain.deadline == std::chrono::milliseconds{30000});
	CHECK(drain.stop_accepting);
	CHECK(drain.close_idle);
	CHECK(drain.finish_requests);
	CHECK_FALSE(drain.finish_streams);
	CHECK(drain.websocket_policy == http::DrainStreamPolicy::close);
	CHECK(drain.sse_policy == http::DrainStreamPolicy::close);

	http::DrainReport report{};
	CHECK(report.accepted_before_stop == 0);
	CHECK_FALSE(report.deadline_hit);

	http::ServerMetrics metrics{};
	CHECK(metrics.pressure.accept_rejected == 0);
	CHECK(metrics.pressure.drain_started == 0);
	CHECK(metrics.pressure.drain_forced_close == 0);
}

struct FacadeAnswer {
	std::string value;
};

struct FacadeSearch {
	std::string q;
	std::uint32_t page{};
};

struct FacadeTodo {
	std::int64_t id{};
	std::string title;
	bool done{};
};

struct FacadeCreateTodo {
	std::string title;
};

struct FacadeTodoList {
	std::vector<FacadeTodo> items;
};

struct FacadeTodoStore {
	std::mutex mu;
	std::vector<FacadeTodo> todos;
	std::int64_t next_id{1};
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

template<>
struct JsonMembers<FacadeSearch> {
	static constexpr auto members() {
		return std::tuple{
			json_member("q", &FacadeSearch::q),
			json_member("page", &FacadeSearch::page),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeSearch"; }
};

template<>
struct JsonMembers<FacadeTodo> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &FacadeTodo::id),
			json_member("title", &FacadeTodo::title),
			json_member("done", &FacadeTodo::done),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeTodo"; }
};

template<>
struct JsonMembers<FacadeCreateTodo> {
	static constexpr auto members() {
		return std::tuple{
			json_member("title", &FacadeCreateTodo::title),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeCreateTodo"; }
};

template<>
struct JsonMembers<FacadeTodoList> {
	static constexpr auto members() {
		return std::tuple{
			json_member("items", &FacadeTodoList::items),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeTodoList"; }
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
	CHECK(report.issues[0].code == "http.route.duplicate");
	CHECK(report.issues[0].message == "duplicate route");
	CHECK(report.issues[0].method == "GET");
	CHECK(report.issues[0].path == "/same");
	CHECK(report.issues[0].source_file.ends_with("http_facade_test.cxx"));
	CHECK(report.issues[0].source_line > 0);
	CHECK(report.issues[0].related_source_file.ends_with("http_facade_test.cxx"));
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
	CHECK(report.issues[0].code == "http.route.ambiguous");
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

	auto mounts = app.static_mounts();
	REQUIRE(mounts.size() == 1);
	CHECK(mounts[0].url_prefix == "/assets");
	CHECK(mounts[0].root_dir == "/tmp/conflux-missing-static-root-for-test");
	CHECK(mounts[0].source_file.ends_with("http_facade_test.cxx"));
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
	"http facade: json CRUD route table snapshot",
	"[http.facade]") {
	auto app = http::app();
	auto store = std::make_shared<FacadeTodoStore>();
	app.state(store);

	app.get("/todos", [](http::State<FacadeTodoStore> todos) {
		std::lock_guard lock{todos->mu};
		return http::Json{FacadeTodoList{.items = todos->todos}};
	});
	app.get<"/todos/{id:i64}">(
		[](http::Path<"id", std::int64_t> id,
		   http::State<FacadeTodoStore> todos) -> std::expected<http::Json<FacadeTodo>, http::Problem> {
			std::lock_guard lock{todos->mu};
			auto it = std::ranges::find(todos->todos, id.get(), &FacadeTodo::id);
			if (it == todos->todos.end()) {
				return std::unexpected{http::problem::not_found("todo_not_found", "todo not found")};
			}
			return http::Json{*it};
		});
	app.post(
		"/todos",
		[](http::Json<FacadeCreateTodo> const &body,
		   http::State<FacadeTodoStore> todos) -> std::expected<http::Created, http::Problem> {
			if (body->title.empty()) {
				return std::unexpected{http::problem::bad_request("invalid_todo", "title is required")};
			}
			std::lock_guard lock{todos->mu};
			auto todo = FacadeTodo{.id = todos->next_id++, .title = body->title};
			todos->todos.push_back(todo);
			return http::created(todo).header("Location", std::format("/todos/{}", todo.id));
		});

	CHECK(
		app.route_table()
		== "GET /todos [app] State\n"
		   "GET /todos/{id:i64} [app] Path<id>,State\n"
		   "POST /todos [app] Json,State");
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
	CHECK(
		app.route_table()
		== "POST /upload [app] name=upload.create max_body=1048576 timeout=5000ms rate_limit=uploads auth=user BodyText");
}

TEST_CASE(
	"http facade: routes record middleware count at registration",
	"[http.facade]") {
	auto app = http::app();
	app.get("/before", [] { return http::no_content(); });
	app.use([](http::RequestView const &req, http::Next const &next) { return next(req); });
	app.get("/after-one", [] { return http::no_content(); });
	app.use([](http::RequestView const &req, http::Next const &next) { return next(req); });
	app.get("/after-two", [] { return http::no_content(); });

	auto routes = app.routes();
	REQUIRE(routes.size() == 3);
	CHECK(routes[0].path == "/before");
	CHECK(routes[0].middleware_count == 0);
	CHECK(routes[1].path == "/after-one");
	CHECK(routes[1].middleware_count == 1);
	CHECK(routes[2].path == "/after-two");
	CHECK(routes[2].middleware_count == 2);
}

TEST_CASE(
	"http facade: observability facade installs request id tracing logs and metrics",
	"[http.facade]") {
	auto app = http::app();
	std::vector<std::string> logs;
	app.use(
		http::observability({
			.service_name = "api",
			.log_request_headers = true,
			.extra_sensitive_headers = {"X-Secret"},
			.access_log_sink = [&](std::string const &line) { logs.push_back(line); },
		}));
	app.get("/items/{id}", [](http::RequestId request_id, http::TraceContext trace) {
		auto response = http::text(std::format("{} {}", request_id.get(), trace.traceparent));
		response.headers.set("Set-Cookie", "session=abc");
		return response;
	});

	Request req;
	req.method = "GET";
	req.path = "/items/42?debug=true";
	req.headers.set("Authorization", "Bearer secret");
	req.headers.set("X-Secret", "secret");

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.headers.get("X-Request-ID").has_value());
	CHECK(response.headers.get("Traceparent").has_value());
	REQUIRE(logs.size() == 1);
	CHECK(logs[0].contains(R"("service":"api")"));
	CHECK(logs[0].contains(R"("route":"/items/{id}")"));
	CHECK(logs[0].contains(R"("path":"/items/42")"));
	CHECK_FALSE(logs[0].contains("debug=true"));
	CHECK(logs[0].contains(R"("Authorization":"<redacted>")"));
	CHECK(logs[0].contains(R"("X-Secret":"<redacted>")"));
	CHECK_FALSE(logs[0].contains("Bearer secret"));

	Request metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	CHECK(metrics.status == kHttpOk);
	CHECK(metrics.text_body().contains("http_requests_total"));
	CHECK(metrics.text_body().contains(
		R"(service="api",route="/items/{id}",method="GET",status_class="2xx",status="200")"));
	CHECK(metrics.text_body().contains("http_request_duration_seconds_count"));
	CHECK_FALSE(metrics.text_body().contains("debug=true"));
}

TEST_CASE(
	"http facade: observability metrics route can be disabled and collision is validated",
	"[http.facade]") {
	auto disabled = http::app();
	disabled.use(http::observability({.access_log = false, .register_metrics_route = false}));
	CHECK(disabled.routes().empty());

	auto colliding = http::app();
	colliding.get("/metrics", [] { return http::text("custom"); });
	colliding.use(http::observability({.access_log = false}));

	auto report = colliding.validate();
	REQUIRE_FALSE(report.ok());
	CHECK(std::ranges::any_of(report.issues, [](auto const &issue) {
		return issue.method == "GET" && issue.path == "/metrics" && issue.message == "duplicate route";
	}));
}

TEST_CASE(
	"http facade: observability records unmatched route metrics and logs",
	"[http.facade]") {
	auto app = http::app();
	std::vector<std::string> logs;
	app.use(
		http::observability({
			.service_name = "api",
			.access_log_sink = [&](std::string const &line) { logs.push_back(line); },
		}));

	Request req;
	req.method = "GET";
	req.path = "/missing?token=secret";
	auto missing = http::router(app).dispatch(req);
	CHECK(missing.status == kHttpNotFound);
	REQUIRE(logs.size() == 1);
	CHECK(logs[0].contains(R"("route":"<unmatched>")"));
	CHECK_FALSE(logs[0].contains("token=secret"));

	Request metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	CHECK(metrics.text_body().contains(R"(route="<unmatched>")"));
	CHECK(metrics.text_body().contains("http_rejections_total"));
}

TEST_CASE(
	"http facade: observability metrics include explicit runtime and work sources",
	"[http.facade]") {
	auto pool = std::make_shared<WorkPool>(WorkPoolOptions{.threads = 1, .max_inject_queue = 16});
	std::atomic<int> ran{0};
	REQUIRE(pool->enqueue([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
	pool->drain_and_stop();
	CHECK(ran.load(std::memory_order_relaxed) == 1);

	HttpPressureMetrics pressure{};
	pressure.accept_rejected = 2;
	pressure.sse_dropped_newest = 3;
	pressure.websocket_closed_for_pressure = 4;

	auto app = http::app();
	app.use(
		http::observability(
			{
				.service_name = "api",
				.access_log = false,
				.task_allocation_metrics = true,
				.work_pools = {{"default", pool}},
			},
			http::ObservabilitySinks{
				.pressure_metrics = [pressure] { return pressure; },
				.json_arena_metrics =
					[] {
						return http::JsonArenaMetrics{
							.slabs_total = 5,
							.high_water_bytes = 4096,
							.allocated_bytes = 2048};
					},
			}));

	Request metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	auto const body = metrics.text_body();
	CHECK(body.contains(R"(http_pressure_overflow_total{kind="accept",policy="reject"} 2)"));
	CHECK(body.contains(R"(http_pressure_overflow_total{kind="sse",policy="drop_newest"} 3)"));
	CHECK(body.contains(R"(work_pool_completed_total{pool="default"})"));
	CHECK(body.contains("work_task_frame_allocations_total"));
	CHECK_FALSE(body.contains("json_arena_slabs_total"));
}

TEST_CASE(
	"http facade: observability JSON arena metrics are opt-in",
	"[http.facade]") {
	auto app = http::app();
	app.use(
		http::observability(
			{
				.access_log = false,
				.json_arena_metrics = true,
			},
			http::ObservabilitySinks{
				.json_arena_metrics =
					[] {
						return http::JsonArenaMetrics{
							.slabs_total = 2,
							.high_water_bytes = 256,
							.allocated_bytes = 128};
					},
			}));

	Request metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	auto const body = metrics.text_body();
	CHECK(body.contains("json_arena_slabs_total 2"));
	CHECK(body.contains("json_arena_high_water_bytes 256"));
	CHECK(body.contains("json_arena_allocated_bytes 128"));
}

TEST_CASE(
	"http facade: observability server rejection hook shares metrics registry",
	"[http.facade]") {
	auto middleware = http::observability({.access_log = false});
	auto hooks = http::observability_server_hooks(middleware);
	REQUIRE(static_cast<bool>(hooks.rejection));
	hooks.rejection(
		HttpRejectReason::header_line_too_large,
		reject_reason_status(HttpRejectReason::header_line_too_large));

	auto app = http::app();
	app.use(middleware);

	Request metrics_req;
	metrics_req.method = "GET";
	metrics_req.path = "/metrics";
	auto metrics = http::router(app).dispatch(metrics_req);
	CHECK(metrics.text_body().contains(
		R"(http_rejections_total{service="conflux",reason="header_line_too_large",status="431"} 1)"));
}

TEST_CASE(
	"http facade: async middleware uses request views",
	"[http.facade]") {
	auto app = http::app();
	app.use(
		[](http::RequestView const &req,
		   http::RequestContext const &ctx,
		   http::AsyncNext const &next) -> http::Task<http::Response> {
			auto response = co_await next(req, ctx);
			response.headers.set("x-async-middleware", "1");
			co_return response;
		});
	app.get("/after-async", [] { return http::no_content(); });

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].middleware_count == 1);
	CHECK(http::router(app).has_context_routes());
}

TEST_CASE(
	"http facade: middleware can observe SSE close",
	"[http.facade]") {
	auto app = http::app();
	auto close_count = std::make_shared<int>(0);
	auto channel = std::make_shared<http::SseChannel>();

	app.use([close_count](http::RequestView const &req, http::Next const &next) {
		auto response = next(req);
		if (response.is_sse()) {
			response.sse_channel_ptr()->on_close([close_count] { ++*close_count; });
		}
		return response;
	});
	app.get("/events", [channel] { return http::sse(channel); });

	Request req;
	req.method = "GET";
	req.path = "/events";

	auto response = http::router(app).dispatch(req);
	REQUIRE(response.is_sse());
	response.sse_channel_ptr()->close();
	response.sse_channel_ptr()->close();
	CHECK(*close_count == 1);
}

TEST_CASE(
	"http facade: ordinary verbs accept context handlers",
	"[http.facade]") {
	auto app = http::app();
	app.get("/context", [](http::RequestView const &, http::RequestContext const &) -> http::Task<http::Response> {
		co_return http::text("context");
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].method == "GET");
	CHECK(routes[0].path == "/context");
	CHECK(routes[0].handler_kind == "context");
	CHECK(routes[0].extractors == std::vector<std::string>{"RequestView"});
	CHECK(http::router(app).has_context_routes());
}

TEST_CASE(
	"http facade: extracted handlers can return tasks",
	"[http.facade]") {
	auto app = http::app();
	std::string value = "async-state";
	app.state(value);

	app.get("/async-state", [](http::State<std::string> state) -> http::Task<http::Json<FacadeAnswer>> {
		co_return http::json(FacadeAnswer{.value = state.get()});
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"State"});
	CHECK(routes[0].produces == std::vector<std::string>{"application/json"});
	CHECK(http::router(app).has_context_routes());
}

TEST_CASE(
	"http facade: app groups apply scoped async middleware to extracted task routes",
	"[http.facade]") {
	auto app = http::app();
	std::string value = "async-state";
	app.state(value);

	app.group("/api", [](auto &group) {
		group.use(
			[](http::RequestView const &req,
			   http::RequestContext const &ctx,
			   http::AsyncNext const &next) -> http::Task<http::Response> {
				auto response = co_await next(req, ctx);
				response.headers.set("x-async-group", "api");
				co_return response;
			});
		(void)group.get("/async-state", [](http::State<std::string> state) -> http::Task<http::Response> {
			co_return http::text(state.get());
		});
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].middleware_count == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"State"});
	CHECK(http::router(app).has_context_routes());
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
	"http facade: app groups preserve extractor routing",
	"[http.facade]") {
	auto app = http::app();
	app.group("/api", [](auto &group) {
		(void)group.get("/items/{id}", [](http::Path<"id", std::uint64_t> id) {
			return http::text(std::format("item={}", id.get()));
		});
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].path == "/api/items/{id}");
	CHECK(routes[0].extractors == std::vector<std::string>{"Path<id>"});

	Request req;
	req.method = "GET";
	req.path = "/api/items/42";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "item=42");
}

TEST_CASE(
	"http facade: app groups apply scoped middleware to extractor routes",
	"[http.facade]") {
	auto app = http::app();
	app.group("/api", [](auto &group) {
		group.use([](http::RequestView const &req, http::Next const &next) {
			auto response = next(req);
			response.headers.set("x-group", "api");
			return response;
		});
		(void)group.get("/items/{id}", [](http::Path<"id", std::uint64_t> id) {
			return http::text(std::format("item={}", id.get()));
		});
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].middleware_count == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"Path<id>"});

	Request req;
	req.method = "GET";
	req.path = "/api/items/42";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.headers.get("x-group") == "api");
	CHECK(response.text_body() == "item=42");
}

TEST_CASE(
	"http facade: app groups support typed route patterns",
	"[http.facade]") {
	auto app = http::app();
	app.group("/api", [](auto &group) {
		(void)group.template get<"/items/{id:u64}">(
			[](http::Path<"id", std::uint64_t> id) { return http::text(std::format("item={}", id.get())); });
	});

	auto report = app.validate();
	CHECK(report);

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].path == "/api/items/{id:u64}");
	CHECK(
		routes[0].path_param_types
		== std::map<std::string, std::string>{
			{"id", "u64"}
    });
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
	"http facade: app openapi spec maps typed path parameters",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/users/{id:u64}">(
		[](http::Path<"id", std::uint64_t> id) { return http::text(std::format("{}", id.get())); });
	app.get<"/teams/{slug}">([](http::Path<"slug"> slug) { return http::text(slug.get()); });

	auto spec = app.openapi_spec();
	CHECK(
		spec.find(
			R"("name":"id","in":"path","required":true,"schema":{"type":"integer","format":"uint64","minimum":0})")
		!= std::string::npos);
	CHECK(spec.find(R"("name":"slug","in":"path","required":true,"schema":{"type":"string"})") != std::string::npos);
}

TEST_CASE(
	"http facade: app openapi spec includes JSON request bodies",
	"[http.facade]") {
	auto app = http::app();
	app.post("/answers", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	auto spec = app.openapi_spec();
	CHECK(spec.find(R"("requestBody")") != std::string::npos);
	CHECK(spec.find(R"("application/json")") != std::string::npos);
	CHECK(spec.find(R"("application/problem+json")") != std::string::npos);
	CHECK(spec.find(R"("properties":{"value":{"type":"string"}})") != std::string::npos);
	CHECK(spec.find(R"("required":["value"])") != std::string::npos);
}

TEST_CASE(
	"http facade: app route metadata records JSON responses",
	"[http.facade]") {
	auto app = http::app();
	app.get("/answer", [] { return http::Json{FacadeAnswer{.value = "ok"}}; });
	app.get("/expected", []() -> std::expected<http::Json<FacadeAnswer>, http::Problem> {
		return http::Json{FacadeAnswer{.value = "ok"}};
	});
	app.post("/created", [] { return http::created(FacadeAnswer{.value = "made"}); });

	auto routes = app.routes();
	REQUIRE(routes.size() == 3);
	CHECK(routes[0].produces == std::vector<std::string>{"application/json"});
	CHECK(routes[1].produces == std::vector<std::string>{"application/json"});
	CHECK(routes[2].success_status == kHttpCreated);
	CHECK_FALSE(routes[0].problem_response);
	CHECK(routes[1].problem_response);
	auto spec = app.openapi_spec();
	CHECK(spec.find(R"("application/json")") != std::string::npos);
	CHECK(spec.find(R"("properties":{"value":{"type":"string"}})") != std::string::npos);
	CHECK(spec.find(R"("201":{"description":"Created")") != std::string::npos);
	CHECK(spec.find(R"("400":{"description":"Problem")") != std::string::npos);
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
	"http facade: app openapi snapshot covers typed route policies",
	"[http.facade]") {
	auto app = http::app();
	app.get<"/widgets/{id:u64}">(
		   [](http::Path<"id", std::uint64_t>) -> std::expected<http::Json<FacadeAnswer>, http::Problem> {
			   return http::Json{FacadeAnswer{.value = "ok"}};
		   })
		.name("widgets.show")
		.openapi_summary("Show widget")
		.auth_policy("user")
		.rate_limit("widgets")
		.timeout(std::chrono::seconds{5});

	CHECK(
		app.openapi_spec("Snapshot API", "1.0.0")
		== R"({"openapi":"3.0.0","info":{"title":"Snapshot API","version":"1.0.0"},"components":{"securitySchemes":{"bearerAuth":{"type":"http","scheme":"bearer"}}},"paths":{"/widgets/{id:u64}":{"get":{"operationId":"widgets.show","summary":"Show widget","security":[{"bearerAuth":[]}],"x-auth-policy":"user","x-timeout-ms":5000,"x-rate-limit":"widgets","parameters":[{"name":"id","in":"path","required":true,"schema":{"type":"integer","format":"uint64","minimum":0}}],"responses":{"200":{"description":"OK","content":{"application/json":{"schema":{"type":"object","properties":{"value":{"type":"string"}},"required":["value"]}}}},"400":{"description":"Problem","content":{"application/problem+json":{"schema":{"type":"object"}}}},"401":{"description":"Unauthorized"},"429":{"description":"Too Many Requests"},"504":{"description":"Gateway Timeout"}}}}})");
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
	"http facade: route auth policy requires bearer credentials",
	"[http.facade]") {
	auto app = http::app();
	app.get("/private", [] { return http::text("secret"); }).auth_policy("user");

	Request req;
	req.method = "GET";
	req.path = "/private";

	auto unauthorized = http::router(app).dispatch(req);
	CHECK(unauthorized.status == kHttpUnauthorized);
	CHECK(unauthorized.headers["WWW-Authenticate"] == "Bearer");

	req.headers["authorization"] = "Bearer token";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == "secret");
}

TEST_CASE(
	"http facade: route auth policy wraps extracted handlers",
	"[http.facade]") {
	auto app = http::app();
	std::string secret = "state-secret";
	app.state(secret);
	app.get("/private-state", [](http::State<std::string> state) { return http::text(state.get()); })
		.auth_policy("user");

	Request req;
	req.method = "GET";
	req.path = "/private-state";

	auto unauthorized = http::router(app).dispatch(req);
	CHECK(unauthorized.status == kHttpUnauthorized);

	req.headers["authorization"] = "Bearer token";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == "state-secret");
}

TEST_CASE(
	"http facade: route rate limit gates repeated requests",
	"[http.facade]") {
	auto app = http::app();
	app.get("/limited", [] { return http::text("ok"); })
		.rate_limit("tiny", http::AppRateLimitOptions{.requests = 1, .window = std::chrono::seconds{60}});

	Request req;
	req.method = "GET";
	req.path = "/limited";
	req.remote_addr = "203.0.113.10";

	auto first = http::router(app).dispatch(req);
	CHECK(first.status == kHttpOk);
	CHECK(first.text_body() == "ok");

	auto second = http::router(app).dispatch(req);
	CHECK(second.status == 429);
	CHECK(second.headers["Retry-After"] == "60");
}

TEST_CASE(
	"http facade: route timeout updates deferred response deadline",
	"[http.facade]") {
	auto deferred = std::make_shared<DeferredResponse>(std::chrono::hours{1});
	auto app = http::app();
	app.get("/deferred", [deferred] { return Response::deferred(deferred); }).timeout(std::chrono::milliseconds{25});

	Request req;
	req.method = "GET";
	req.path = "/deferred";

	auto const before = std::chrono::steady_clock::now();
	auto response = http::router(app).dispatch(req);
	REQUIRE(response.is_deferred());
	auto const deadline = response.deferred_response_ptr()->deadline();
	CHECK(deadline >= before);
	CHECK(deadline <= before + std::chrono::seconds{1});
}

TEST_CASE(
	"http facade: app openapi spec includes route-local policy metadata",
	"[http.facade]") {
	auto app = http::app();
	app.use([](http::RequestView const &req, http::Next const &next) { return next(req); });
	app.post("/upload", [](http::BodyText) { return http::no_content(); })
		.timeout(std::chrono::seconds{5})
		.rate_limit("uploads")
		.auth_policy("user")
		.max_body_size(4096);

	auto spec = app.openapi_spec();
	CHECK(spec.find(R"("x-timeout-ms":5000)") != std::string::npos);
	CHECK(spec.find(R"("x-rate-limit":"uploads")") != std::string::npos);
	CHECK(spec.find(R"("x-max-body-size":4096)") != std::string::npos);
	CHECK(spec.find(R"("x-middleware-count":1)") != std::string::npos);
	CHECK(spec.find(R"("401":{"description":"Unauthorized"})") != std::string::npos);
	CHECK(spec.find(R"("429":{"description":"Too Many Requests"})") != std::string::npos);
	CHECK(spec.find(R"("504":{"description":"Gateway Timeout"})") != std::string::npos);
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

	Request req;
	req.method = "GET";
	req.path = "/openapi.json";

	auto response = http::router(app).dispatch(req);
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
	CHECK(server.error() == "GET /needs-state [app.state.missing]: missing app state");
}

TEST_CASE(
	"http facade: listen validates before creating server",
	"[http.facade]") {
	auto app = http::app();
	app.get("/needs-state", [](http::State<std::string> state) { return http::text(state.get()); });

	auto server = std::move(app).listen();
	REQUIRE_FALSE(server.has_value());
	CHECK(server.error() == "GET /needs-state [app.state.missing]: missing app state");
}

TEST_CASE(
	"http facade: server startup report is explicit and redacted",
	"[http.facade]") {
	auto cfg = http::Config::development();
	cfg.port = 0;
	cfg.startup_banner = false;
	cfg.request_timeout_ms = 1000;
	cfg.dump_effective_config = true;
	cfg.send_fixed_buffers = true;
	cfg.fixed_buffer_slabs = 1024 * 1024;
	cfg.fixed_buffer_bytes = 1024 * 1024;
	cfg.feature_fallback = conflux::runtime::FeatureFallback::silent_fallback;
	cfg.auth_secrets.jwt.active = SecretSource{.kind = SecretSourceKind::literal, .value = "super-secret-token"};
	auto app = http::App{cfg};
	app.get("/health", [] { return http::text("ok"); });

	auto server = std::move(app).try_server();
	if (!server) {
		FAIL_CHECK(server.error());
	}
	REQUIRE(server.has_value());
	auto report = (*server)->startup_report();
	CHECK(report.find("Build:") != std::string::npos);
	CHECK(report.find("Capabilities:") != std::string::npos);
	CHECK(report.find("Fallbacks:") != std::string::npos);
	CHECK(report.find("Config:") != std::string::npos);
	CHECK(report.find("\"server\"") != std::string::npos);
	CHECK(report.find("policy=silent_fallback") != std::string::npos);
	CHECK(report.find("super-secret-token") == std::string::npos);
}

TEST_CASE(
	"http facade: app handlers can return Json wrappers",
	"[http.facade]") {
	auto app = http::app();
	app.get("/answer", [] { return http::Json{FacadeAnswer{.value = "ok"}}; });

	Request req;
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

	Request req;
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

	Request req;
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
		== std::map<std::string, std::string>{
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
		== std::map<std::string, std::string>{
			{"id", "u64"}
    });
	CHECK(app.validate().ok());

	Request req;
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
		== std::map<std::string, std::string>{
			{"id", "i64"}
    });
	CHECK(app.validate().ok());

	Request req;
	req.method = "GET";
	req.path = "/todos/-42";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "todo=-42");

	req.path = "/todos/nope";
	auto bad = http::router(app).dispatch(req);
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.content_type == "application/problem+json");
	CHECK(bad.text_body().contains(R"("extractor":"Path")"));
	CHECK(bad.text_body().contains(R"("name":"id")"));
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

	Request req;
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

	Request req;
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

	Request req;
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

	Request req;
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

	Request req;
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
	app.get("/bearer", [](http::Bearer bearer) { return http::text(bearer.get()); });

	Request req;
	req.method = "GET";
	req.path = "/bearer";
	req.headers["authorization"] = "bearer  token-123 \t";

	auto response = http::router(app).dispatch(req);
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

	Request req;
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
	"http facade: typed field extractors parse scalars and reject malformed values",
	"[http.facade]") {
	auto app = http::app();
	app.get("/items/{id}", [](http::Path<"id", std::uint64_t> id, http::Query<"page", std::uint32_t> page) {
		return http::text(std::format("{}:{}", id.get(), page.get()));
	});

	Request req;
	req.method = "GET";
	req.path = "/items/42";
	req.params["id"] = "42";
	req.query["page"] = "7";
	CHECK(http::router(app).dispatch(req).text_body() == "42:7");

	req.query["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.content_type == "application/problem+json");
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

	Request req;
	req.method = "POST";
	req.path = "/submit";
	req.form["name"] = "Ada";
	req.form["age"] = "37";
	CHECK(http::router(app).dispatch(req).text_body() == "Ada:37");

	req.form["age"] = "bad";
	auto bad = http::router(app).dispatch(req);
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.content_type == "application/problem+json");
	CHECK(bad.text_body().find(R"("extractor":"Form")") != std::string_view::npos);
	CHECK(bad.text_body().find(R"("name":"age")") != std::string_view::npos);
	CHECK(bad.text_body().find(R"("kind":"invalid")") != std::string_view::npos);
}

TEST_CASE(
	"http facade: app handlers can receive aggregate query params",
	"[http.facade]") {
	auto app = http::app();
	app.get("/search", [](http::QueryParams<FacadeSearch> search) {
		return http::text(std::format("{}:{}", search->q, search->page));
	});

	Request req;
	req.method = "GET";
	req.path = "/search";
	req.query["q"] = "conflux";
	req.query["page"] = "3";
	CHECK(http::router(app).dispatch(req).text_body() == "conflux:3");

	req.query["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.content_type == "application/problem+json");
	CHECK(bad.text_body().find(R"("extractor":"QueryParams")") != std::string_view::npos);
	CHECK(bad.text_body().find(R"("name":"page")") != std::string_view::npos);

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"QueryParams"});
}

TEST_CASE(
	"http facade: app handlers can receive aggregate form params",
	"[http.facade]") {
	auto app = http::app();
	app.post("/search", [](http::FormParams<FacadeSearch> search) {
		return http::text(std::format("{}:{}", search->q, search->page));
	});

	Request req;
	req.method = "POST";
	req.path = "/search";
	req.form["q"] = "conflux";
	req.form["page"] = "4";
	CHECK(http::router(app).dispatch(req).text_body() == "conflux:4");

	req.form["page"] = "bad";
	auto bad = http::router(app).dispatch(req);
	CHECK(bad.status == kHttpBadRequest);
	CHECK(bad.content_type == "application/problem+json");
	CHECK(bad.text_body().find(R"("extractor":"FormParams")") != std::string_view::npos);
	CHECK(bad.text_body().find(R"("name":"page")") != std::string_view::npos);

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"FormParams"});
}

TEST_CASE(
	"http facade: JSON body routes validate content type",
	"[http.facade]") {
	auto app = http::app();
	app.post("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; });

	Request req;
	req.method = "POST";
	req.path = "/json";
	req.body = R"({"value":"ok"})";

	auto missing = http::router(app).dispatch(req);
	CHECK(missing.status == kHttpBadRequest);
	CHECK(missing.content_type == "application/problem+json");
	CHECK(missing.text_body().find(R"("code":"unsupported_content_type")") != std::string_view::npos);
	CHECK(missing.text_body().find(R"("expected":"application/json")") != std::string_view::npos);

	req.headers["content-type"] = "application/json";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == R"({"value":"ok"})");

	req.body = R"({"value":)";
	auto malformed = http::router(app).dispatch(req);
	CHECK(malformed.status == kHttpBadRequest);
	CHECK(malformed.content_type == "application/problem+json");
	CHECK(malformed.text_body().find(R"("code":"json.decode.type_mismatch")") != std::string_view::npos);
	CHECK(malformed.text_body().find(R"("stage":"parse")") != std::string_view::npos);
	CHECK(malformed.text_body().find(R"("kind":"unexpected_eof")") != std::string_view::npos);
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

	Request req;
	req.method = "POST";
	req.path = "/json-doc";
	req.body = R"({"value":"ok"})";

	auto missing = http::router(app).dispatch(req);
	CHECK(missing.status == kHttpBadRequest);
	CHECK(missing.content_type == "application/problem+json");
	CHECK(missing.text_body().find(R"("code":"unsupported_content_type")") != std::string_view::npos);
	CHECK(missing.text_body().find(R"("expected":"application/json")") != std::string_view::npos);

	req.headers["content-type"] = "application/json";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == R"({"value":"ok"})");

	req.body = R"({"value":)";
	auto malformed = http::router(app).dispatch(req);
	CHECK(malformed.status == kHttpBadRequest);
	CHECK(malformed.content_type == "application/problem+json");
	CHECK(malformed.text_body().find(R"("code":"json.decode.type_mismatch")") != std::string_view::npos);

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

	Request req;
	req.method = "POST";
	req.path = "/json-doc";
	req.headers["content-type"] = "application/json";
	req.body = R"({"value":"too large"})";

	auto too_large = http::router(app).dispatch(req);
	CHECK(too_large.status == kHttpRequestEntityTooLarge);
	CHECK(too_large.content_type == "application/problem+json");
	CHECK(too_large.text_body().find(R"("code":"body_too_large")") != std::string_view::npos);
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

	Request req;
	req.method = "PATCH";
	req.path = "/patch";
	req.body = R"([{"op":"add","path":"/name","value":"Ada"}])";

	auto wrong = http::router(app).dispatch(req);
	CHECK(wrong.status == kHttpBadRequest);
	CHECK(wrong.content_type == "application/problem+json");
	CHECK(wrong.text_body().find(R"("code":"unsupported_content_type")") != std::string_view::npos);

	req.headers["content-type"] = "application/json-patch+json";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpNoContent);

	req.body = R"([{"path":"/name","value":"Ada"}])";
	auto invalid = http::router(app).dispatch(req);
	CHECK(invalid.status == kHttpBadRequest);
	CHECK(invalid.content_type == "application/problem+json");
	CHECK(invalid.text_body().find(R"("code":"patch_op_missing")") != std::string_view::npos);
	CHECK(invalid.text_body().find(R"("stage":"json_patch")") != std::string_view::npos);

	auto spec = app.openapi_spec("Patch API", "1.0");
	CHECK(spec.find("application/json-patch+json") != std::string::npos);
	CHECK(spec.find(R"("required":["op","path"])") != std::string::npos);
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

	Request req;
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

	req.body = R"({"long":true})";
	auto too_large = http::router(app).dispatch(req);
	CHECK(too_large.status == kHttpRequestEntityTooLarge);
	CHECK(too_large.content_type == "application/problem+json");

	auto spec = app.openapi_spec("Merge API", "1.0");
	CHECK(spec.find("application/merge-patch+json") != std::string::npos);
}

TEST_CASE(
	"http facade: JSON body routes enforce route-local body limits",
	"[http.facade]") {
	auto app = http::app();
	app.post("/json", [](http::Json<FacadeAnswer> const &body) { return http::Json{*body}; }).max_body_size(8);

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].max_body_size == 8);

	Request req;
	req.method = "POST";
	req.path = "/json";
	req.headers["content-type"] = "application/json";
	req.body = R"({"value":"too large"})";

	auto too_large = http::router(app).dispatch(req);
	CHECK(too_large.status == kHttpRequestEntityTooLarge);
	CHECK(too_large.status_text == "Content Too Large");
	CHECK(too_large.content_type == "application/problem+json");
	CHECK(too_large.text_body().find(R"("code":"body_too_large")") != std::string_view::npos);
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

	Request req;
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

	Request req;
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

	Request req;
	req.method = "POST";
	req.path = "/upload";
	req.form["title"] = "Report";
	req.files.push_back(UploadedFile::borrowed("upload", "report.txt", "text/plain", "file-body"));

	auto response = http::router(app).dispatch(req);
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
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() {
			std::error_code ec;
			(void)std::filesystem::remove(path, ec);
		}
	};
	Cleanup file{
		.path =
			std::filesystem::temp_directory_path() / std::format("conflux_http_facade_file_helper_{}.txt", ::getpid())};
	auto const &path = file.path;
	{
		std::ofstream out{path, std::ios::binary};
		out << "file-body";
	}

	auto response = http::file(path, "text/plain");
	CHECK(response.status == kHttpOk);
	CHECK(response.content_type == "text/plain");
	CHECK(response.text_body() == "file-body");
}

TEST_CASE(
	"http facade: problem helpers carry code/detail metadata",
	"[http.facade]") {
	auto problem = http::problem::bad_request("invalid_todo", "title is required");
	CHECK(problem.code == "invalid_todo");
	CHECK(problem.detail == "title is required");
	CHECK(problem.response.status == kHttpBadRequest);
	CHECK(problem.response.content_type == "application/problem+json");
	CHECK(problem.response.text_body() == R"({"code":"invalid_todo","detail":"title is required"})");

	CHECK(http::problem::not_found("missing", "not found").response.status == kHttpNotFound);
	CHECK(http::problem::unauthorized("login_required", "sign in").response.status == kHttpUnauthorized);
	CHECK(http::problem::forbidden("forbidden", "no access").response.status == kHttpForbidden);
	CHECK(http::problem::unprocessable_entity("invalid_entity", "invalid").response.status == kHttpUnprocessableEntity);
	CHECK(http::problem::content_too_large().response.status == kHttpRequestEntityTooLarge);
	CHECK(http::problem::uri_too_long().response.status == kHttpUriTooLong);
	CHECK(http::problem::header_fields_too_large().response.status == kHttpRequestHeaderFieldsTooLarge);
	CHECK(http::problem::gateway_timeout().response.status == kHttpGatewayTimeout);
	CHECK(http::problem::internal_error("internal", "failed").response.status == kHttpInternalServerError);
}
