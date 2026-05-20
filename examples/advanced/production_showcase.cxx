// Production-style HTTP showcase: JSON CRUD, SSE notifications, metrics,
// security policy, and graceful drain.
//
// Build and run: build/release-clang-libcxx/conflux_production_showcase_example
// Try:
//   curl -i http://localhost:9105/health
//   curl -i http://localhost:9105/todos
//   curl -i -X POST http://localhost:9105/todos -H 'Content-Type: application/json' -d '{"title":"ship"}'
//   curl -H 'Authorization: Bearer metrics-token' http://localhost:9105/metrics
#include <csignal>
#include <cstdio>

import conflux.http;
import std;

namespace http = conflux::http;
using namespace std::chrono_literals;

struct Todo {
	std::int64_t id{};
	std::string title;
	bool done{};
};

struct TodoList {
	std::vector<Todo> items;
};

struct CreateTodo {
	std::string title;
};

struct TodoStore {
	std::mutex mu;
	std::vector<Todo> todos;
	std::int64_t next_id{1};
};

template<>
struct JsonMembers<Todo> {
	static constexpr auto members() {
		return std::tuple{
			json_member("id", &Todo::id),
			json_member("title", &Todo::title),
			json_member("done", &Todo::done),
		};
	}
	static constexpr std::string_view type_name() { return "Todo"; }
};

template<>
struct JsonMembers<TodoList> {
	static constexpr auto members() { return std::tuple{json_member("items", &TodoList::items)}; }
	static constexpr std::string_view type_name() { return "TodoList"; }
};

template<>
struct JsonMembers<CreateTodo> {
	static constexpr auto members() { return std::tuple{json_member("title", &CreateTodo::title)}; }
	static constexpr std::string_view type_name() { return "CreateTodo"; }
};

namespace {

volatile sig_atomic_t stop_requested = 0;

void handle_stop_signal(
	int) {
	stop_requested = 1;
}

} // namespace

int main() {
	MetricsRegistry metrics;
	auto app = http::app(http::Config::public_server());
	auto store = std::make_shared<TodoStore>();
	auto events =
		std::make_shared<http::SseChannel>(http::SseChannel::kDefaultMaxQueueBytes, SseOverflowPolicy::DropNewest);

	app.state(store);
	app.use(request_id_middleware());
	app.use(metrics_middleware(metrics));
	app.use(security_headers_middleware({
		.hsts_max_age = 0,
		.csp = "default-src 'none'; frame-ancestors 'none'",
	}));
	app.use(cache_control_middleware({.default_directive = "no-store"}));
	app.use(rate_limit_middleware({.requests = 120, .window = 60s, .burst = 20}));

	app.get("/health", [] { return http::text("ok"); });
	app.get("/", [] { return http::json(TodoList{}); });

	app.get("/todos", [](http::State<TodoStore> store) {
		std::lock_guard lock{store->mu};
		return http::json(TodoList{.items = store->todos});
	});

	app.get<"/todos/{id:i64}">(
		[](http::Path<"id", std::int64_t> id,
		   http::State<TodoStore> store) -> std::expected<http::Json<Todo>, http::Problem> {
			std::lock_guard lock{store->mu};
			auto it = std::ranges::find(store->todos, id.get(), &Todo::id);
			if (it == store->todos.end()) {
				return std::unexpected{http::problem::not_found("todo_not_found", "todo not found")};
			}
			return http::json(*it);
		});

	app.post(
		"/todos",
		[events](http::Json<CreateTodo> const &body, http::State<TodoStore> store)
			-> std::expected<http::Created, http::Problem> {
			if (body->title.empty()) {
				return std::unexpected{http::problem::bad_request("invalid_todo", "title is required")};
			}

			Todo todo;
			{
				std::lock_guard lock{store->mu};
				todo = Todo{.id = store->next_id++, .title = body->title};
				store->todos.push_back(todo);
			}
			(void)events->send_event("todo.created", std::format("{}", todo.id));
			return http::created(todo).header("Location", std::format("/todos/{}", todo.id));
		});

	app.get("/events", [events] { return http::sse(events); });

	std::vector<http::Router::Middleware> metrics_auth;
	metrics_auth.push_back(bearer_auth_middleware([](std::string_view token) { return token == "metrics-token"; }));
	app.get("/metrics", metrics_handler_protected(metrics, std::move(metrics_auth)));

	auto server = std::move(app).listen({.port = 9105});
	if (!server) {
		std::println(stderr, "server setup failed: {}", server.error());
		return 1;
	}

	::signal(SIGINT, handle_stop_signal);
	::signal(SIGTERM, handle_stop_signal);

	std::jthread signal_thread{[&] {
		while (stop_requested == 0) {
			std::this_thread::sleep_for(100ms);
		}
		auto report = (*server)->drain(
			http::DrainOptions{
				.deadline = 30s,
				.stop_accepting = true,
				.close_idle = true,
				.finish_requests = true,
				.finish_streams = false,
				.sse_policy = http::DrainStreamPolicy::close_with_retry,
			});
		std::println(
			stderr,
			"drain: finished={} streams_closed={} forced={} deadline_hit={}",
			report.requests_finished,
			report.streams_closed,
			report.forced_closed,
			report.deadline_hit);
	}};

	return static_cast<int>((*server)->run());
}
