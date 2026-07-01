#include <catch2/catch_test_macros.hpp>

import std;
import conflux.http;
import conflux.json;
import conflux.work;

namespace http = conflux::http;
using namespace conflux::http;
using namespace conflux::json;

struct FacadeAnswer {
	std::string value;
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
struct conflux::json::JsonMembers<FacadeAnswer> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("value", &FacadeAnswer::value),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeAnswer"; }
};

template<>
struct conflux::json::JsonMembers<FacadeTodo> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("id", &FacadeTodo::id),
			conflux::json::json_member("title", &FacadeTodo::title),
			conflux::json::json_member("done", &FacadeTodo::done),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeTodo"; }
};

template<>
struct conflux::json::JsonMembers<FacadeCreateTodo> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("title", &FacadeCreateTodo::title),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeCreateTodo"; }
};

template<>
struct conflux::json::JsonMembers<FacadeTodoList> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("items", &FacadeTodoList::items),
		};
	}
	static constexpr std::string_view type_name() { return "FacadeTodoList"; }
};

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
	CHECK(routes[0].source_file.ends_with("http_facade_routes_test.cxx"));
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
		   http::State<FacadeTodoStore> todos) -> http::Result<http::Json<FacadeTodo>> {
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
		   http::State<FacadeTodoStore> todos) -> http::Result<http::CreatedBody<FacadeTodo>> {
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
		   "POST /todos [app] Json,State body_mode=buffered_raw");
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
		.require_bearer_token("user")
		.openapi_summary("Upload a small body");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].method == "POST");
	CHECK(routes[0].path == "/upload");
	CHECK(routes[0].name == "upload.create");
	CHECK(routes[0].max_body_size == 1024 * 1024);
	CHECK(routes[0].timeout == std::chrono::seconds{5});
	CHECK(routes[0].rate_limit == "uploads");
	CHECK(routes[0].bearer_token_policy == "user");
	CHECK(routes[0].openapi_summary == "Upload a small body");
	CHECK(
		app.route_table()
		== "POST /upload [app] name=upload.create max_body=1048576 timeout=5000ms rate_limit=uploads "
		   "bearer_token=user BodyText body_mode=buffered_raw");
}

TEST_CASE(
	"http facade: routes record middleware count at registration",
	"[http.facade]") {
	auto app = http::app();
	app.get("/before", [] { return http::no_content(); });
	app.use([](http::RequestView const &req, auto const &next) { return next(req); });
	app.get("/after-one", [] { return http::no_content(); });
	app.use([](http::RequestView const &req, auto const &next) { return next(req); });
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
	"http facade: async middleware uses request views",
	"[http.facade]") {
	auto app = http::app();
	app.use(
		[](http::RequestView const &req,
		   http::RequestContext const &ctx,
		   auto const &next) -> conflux::work::Task<http::Response> {
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

	app.use([close_count](http::RequestView const &req, auto const &next) {
		auto response = next(req);
		if (response.is_sse()) {
			response.sse_channel_ptr()->on_close([close_count] { ++*close_count; });
		}
		return response;
	});
	app.get("/events", [channel] { return http::sse(channel); });

	conflux::http::OwnedRequest req;
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
	app.get(
		"/context",
		[](http::RequestView const &, http::RequestContext const &) -> conflux::work::Task<http::Response> {
			co_return http::text("context");
		});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].method == "GET");
	CHECK(routes[0].path == "/context");
	CHECK(routes[0].handler_kind == "context");
	CHECK(routes[0].extractors == std::vector<std::string>{"conflux::http::RequestView"});
	CHECK(http::router(app).has_context_routes());
}

TEST_CASE(
	"http facade: extracted handlers can return tasks",
	"[http.facade]") {
	auto app = http::app();
	std::string value = "async-state";
	app.state(value);

	app.get("/async-state", [](http::State<std::string> state) -> conflux::work::Task<http::Json<FacadeAnswer>> {
		co_return http::json(FacadeAnswer{.value = state.get()});
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].extractors == std::vector<std::string>{"State"});
	CHECK(routes[0].produces == std::vector<std::string>{"application/json"});
	CHECK(http::router(app).has_context_routes());
}

TEST_CASE(
	"http facade: async json route keeps response options alive after suspension",
	"[http.facade]") {
	auto app = http::app();
	std::string value = "async-json";
	app.state(value);
	auto source_slot = std::make_shared<std::optional<conflux::work::root::TaskSource<http::Json<FacadeAnswer>>>>();
	app.get(
		"/async-json",
		[source_slot](http::State<std::string> state) -> conflux::work::Task<http::Json<FacadeAnswer>> {
			auto task_source = conflux::work::root::make_task_source<http::Json<FacadeAnswer>>();
			auto task = std::move(task_source.first);
			auto source = std::move(task_source.second);
			source_slot->emplace(std::move(source));
			auto _ = state.get();
			co_return co_await std::move(task);
		});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/async-json";

	auto dispatched = http::router(app).dispatch_context(req, http::RequestContext{});
	REQUIRE(dispatched.has_value());
	REQUIRE(dispatched->is_deferred());
	REQUIRE(source_slot->has_value());

	REQUIRE((*source_slot)
				->try_set_value(
					conflux::work::root::Success<http::Json<FacadeAnswer>>{
						http::json(FacadeAnswer{.value = "async-json"})}));
	auto deferred = dispatched->deferred_response_ptr();
	REQUIRE(deferred->is_ready());
	auto response = deferred->take_ready();
	REQUIRE(response.has_value());
	CHECK(response->status == 200);
	CHECK(response->text_body() == R"({"value":"async-json"})");
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
			   auto const &next) -> conflux::work::Task<http::Response> {
				auto response = co_await next(req, ctx);
				response.headers.set("x-async-group", "api");
				co_return response;
			});
		(void)group.get("/async-state", [](http::State<std::string> state) -> conflux::work::Task<http::Response> {
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

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/api/items/42";

	auto response = http::router(app).dispatch(req);
	CHECK(response.status == kHttpOk);
	CHECK(response.text_body() == "item=42");
}

TEST_CASE(
	"http facade: app groups join slashes safely",
	"[http.facade]") {
	auto app = http::app();
	app.group("/api/", [](auto &group) {
		(void)group.get("items", [] { return http::text("items"); });
		(void)group.get("/users", [] { return http::text("users"); });
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 2);
	CHECK(routes[0].path == "/api/items");
	CHECK(routes[1].path == "/api/users");

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/api/items";
	CHECK(http::router(app).dispatch(req).text_body() == "items");
	req.path = "/api/users";
	CHECK(http::router(app).dispatch(req).text_body() == "users");
}

TEST_CASE(
	"http facade: app groups apply scoped middleware to extractor routes",
	"[http.facade]") {
	auto app = http::app();
	app.group("/api", [](auto &group) {
		group.use([](http::RequestView const &req, auto const &next) {
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

	conflux::http::OwnedRequest req;
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
		== std::vector<std::pair<std::string, std::string>>{
			{"id", "u64"}
    });
}

TEST_CASE(
	"http facade: app group policies apply to nested routes",
	"[http.facade]") {
	auto app = http::app();
	app.group("/api", [](auto &api) {
		api.require_bearer_token("user")
			.rate_limit("api", {.requests = 1, .window = std::chrono::seconds{60}})
			.timeout(std::chrono::milliseconds{250})
			.max_body_size(32);
		api.group("v1", [](auto &v1) {
			(void)v1.post("items", [](http::BodyText body) { return http::text(body.get()); });
		});
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].path == "/api/v1/items");
	CHECK(routes[0].bearer_token_policy == "user");
	CHECK(routes[0].rate_limit == "api");
	CHECK(routes[0].timeout == std::chrono::milliseconds{250});
	CHECK(routes[0].max_body_size == 32);

	conflux::http::OwnedRequest req;
	req.method = "POST";
	req.path = "/api/v1/items";
	req.body = "hello";

	auto unauthorized = http::router(app).dispatch(req);
	CHECK(unauthorized.status == kHttpUnauthorized);

	req.headers["authorization"] = "Bearer token";
	auto ok = http::router(app).dispatch(req);
	CHECK(ok.status == kHttpOk);
	CHECK(ok.text_body() == "hello");

	auto limited = http::router(app).dispatch(req);
	CHECK(limited.status == kHttpTooManyRequests);
}

TEST_CASE(
	"http facade: app group policies protect raw async request-view routes",
	"[http.facade]") {
	auto app = http::app();
	app.group("/api", [](auto &api) {
		api.require_bearer_token("user").rate_limit("api", {.requests = 1, .window = std::chrono::seconds{60}});
		(void)api.get("/raw-async", [](http::RequestView const &) -> conflux::work::Task<http::Response> {
			co_return http::text("secret");
		});
	});

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].path == "/api/raw-async");
	CHECK(routes[0].handler_kind == "app");
	CHECK(routes[0].bearer_token_policy == "user");
	CHECK(routes[0].rate_limit == "api");

	auto resolve = [](http::Response response) {
		if (!response.is_deferred()) {
			return response;
		}
		auto deferred = response.deferred_response_ptr();
		REQUIRE(deferred);
		REQUIRE(deferred->is_ready());
		auto completed = deferred->take_ready();
		REQUIRE(completed.has_value());
		return std::move(*completed);
	};

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/api/raw-async";
	req.remote_addr = "203.0.113.50";

	auto denied = http::router(app).dispatch_context(req, http::RequestContext{});
	REQUIRE(denied.has_value());
	CHECK(resolve(std::move(*denied)).status == kHttpUnauthorized);

	req.headers["authorization"] = "Bearer token";
	auto first = http::router(app).dispatch_context(req, http::RequestContext{});
	REQUIRE(first.has_value());
	auto first_response = resolve(std::move(*first));
	CHECK(first_response.status == kHttpOk);
	CHECK(first_response.text_body() == "secret");

	auto second = http::router(app).dispatch_context(req, http::RequestContext{});
	REQUIRE(second.has_value());
	CHECK(resolve(std::move(*second)).status == kHttpTooManyRequests);
}

TEST_CASE(
	"http facade: route auth policy protects raw async owned-request routes",
	"[http.facade]") {
	auto app = http::app();
	app.get(
		   "/raw-owned",
		   [](http::OwnedRequest const &req) -> conflux::work::Task<http::Response> { co_return http::text(req.path); })
		.require_bearer_token("user");

	auto routes = app.routes();
	REQUIRE(routes.size() == 1);
	CHECK(routes[0].handler_kind == "app");
	CHECK(routes[0].bearer_token_policy == "user");

	auto resolve = [](http::Response response) {
		if (!response.is_deferred()) {
			return response;
		}
		auto deferred = response.deferred_response_ptr();
		REQUIRE(deferred);
		REQUIRE(deferred->is_ready());
		auto completed = deferred->take_ready();
		REQUIRE(completed.has_value());
		return std::move(*completed);
	};

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/raw-owned";

	auto denied = http::router(app).dispatch_context(req, http::RequestContext{});
	REQUIRE(denied.has_value());
	CHECK(resolve(std::move(*denied)).status == kHttpUnauthorized);

	req.headers["authorization"] = "Bearer token";
	auto ok = http::router(app).dispatch_context(req, http::RequestContext{});
	REQUIRE(ok.has_value());
	auto ok_response = resolve(std::move(*ok));
	CHECK(ok_response.status == kHttpOk);
	CHECK(ok_response.text_body() == "/raw-owned");
}

TEST_CASE(
	"http facade: route auth policy requires bearer credentials",
	"[http.facade]") {
	auto app = http::app();
	app.get("/private", [] { return http::text("secret"); }).require_bearer_token("user");

	conflux::http::OwnedRequest req;
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
		.require_bearer_token("user");

	conflux::http::OwnedRequest req;
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

	conflux::http::OwnedRequest req;
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
	"http facade: route rate limit canonicalizes IP keys",
	"[http.facade]") {
	auto app = http::app();
	app.get("/limited", [] { return http::text("ok"); })
		.rate_limit("tiny", http::AppRateLimitOptions{.requests = 1, .window = std::chrono::seconds{60}});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/limited";
	req.remote_addr = "0:0:0:0:0:0:0:1";
	CHECK(http::router(app).dispatch(req).status == kHttpOk);

	req.remote_addr = "::1";
	CHECK(http::router(app).dispatch(req).status == kHttpTooManyRequests);
}

TEST_CASE(
	"http facade: route rate limit canonicalizes endpoint remote addresses",
	"[http.facade]") {
	auto app = http::app();
	app.get("/limited", [] { return http::text("ok"); })
		.rate_limit("tiny", http::AppRateLimitOptions{.requests = 1, .window = std::chrono::seconds{60}});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/limited";
	req.remote_addr = "203.0.113.10:40000";
	CHECK(http::router(app).dispatch(req).status == kHttpOk);

	req.remote_addr = "203.0.113.10:40001";
	CHECK(http::router(app).dispatch(req).status == kHttpTooManyRequests);

	auto ipv6_app = http::app();
	ipv6_app.get("/limited", [] { return http::text("ok"); })
		.rate_limit("tiny", http::AppRateLimitOptions{.requests = 1, .window = std::chrono::seconds{60}});

	req.remote_addr = "[::1]:40000";
	CHECK(http::router(ipv6_app).dispatch(req).status == kHttpOk);

	req.remote_addr = "[0:0:0:0:0:0:0:1]:40001";
	CHECK(http::router(ipv6_app).dispatch(req).status == kHttpTooManyRequests);
}

TEST_CASE(
	"http facade: route rate limit evicts least recently used client",
	"[http.facade]") {
	auto app = http::app();
	app.get("/limited", [] { return http::text("ok"); })
		.rate_limit(
			"tiny",
			http::AppRateLimitOptions{.requests = 1, .window = std::chrono::seconds{60}, .max_clients = 2});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/limited";

	req.remote_addr = "203.0.113.10";
	CHECK(http::router(app).dispatch(req).status == kHttpOk);
	req.remote_addr = "203.0.113.11";
	CHECK(http::router(app).dispatch(req).status == kHttpOk);

	req.remote_addr = "203.0.113.10";
	CHECK(http::router(app).dispatch(req).status == kHttpTooManyRequests);

	req.remote_addr = "203.0.113.12";
	CHECK(http::router(app).dispatch(req).status == kHttpOk);

	req.remote_addr = "203.0.113.10";
	CHECK(http::router(app).dispatch(req).status == kHttpTooManyRequests);
	req.remote_addr = "203.0.113.11";
	CHECK(http::router(app).dispatch(req).status == kHttpOk);
}

TEST_CASE(
	"http facade: route timeout updates deferred response deadline",
	"[http.facade]") {
	auto deferred = std::make_shared<conflux::http::DeferredResponse>(std::chrono::hours{1});
	auto app = http::app();
	app.get("/deferred", [deferred] { return conflux::http::Response::deferred(deferred); })
		.timeout(std::chrono::milliseconds{25});

	conflux::http::OwnedRequest req;
	req.method = "GET";
	req.path = "/deferred";

	auto const before = std::chrono::steady_clock::now();
	auto response = http::router(app).dispatch(req);
	REQUIRE(response.is_deferred());
	auto const deadline = response.deferred_response_ptr()->deadline();
	CHECK(deadline >= before);
	CHECK(deadline <= before + std::chrono::seconds{1});
}
