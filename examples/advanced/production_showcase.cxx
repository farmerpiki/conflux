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

import conflux.extended;
import conflux.net.http.realtime;
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

template<>
struct conflux::json::JsonMembers<Todo> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("id", &Todo::id),
			conflux::json::json_member("title", &Todo::title),
			conflux::json::json_member("done", &Todo::done),
		};
	}
	static constexpr std::string_view type_name() { return "Todo"; }
};

template<>
struct conflux::json::JsonMembers<TodoList> {
	static constexpr auto members() { return std::tuple{conflux::json::json_member("items", &TodoList::items)}; }
	static constexpr std::string_view type_name() { return "TodoList"; }
};

template<>
struct conflux::json::JsonMembers<CreateTodo> {
	static constexpr auto members() { return std::tuple{conflux::json::json_member("title", &CreateTodo::title)}; }
	static constexpr std::string_view type_name() { return "CreateTodo"; }
};

class TodoService {
public:
	TodoService() = default;
	~TodoService() { pool_.drain_and_stop(); }

	TodoService(TodoService const &) = delete;
	TodoService &operator =(TodoService const &) = delete;

	[[nodiscard]] http::Response list() {
		return http::offload(pool_, [this] {
			TodoList list;
			{
				std::scoped_lock lock{mutex_};
				list.items = todos_;
			}
			return http::json(std::move(list));
		});
	}

	[[nodiscard]] http::Response get(
		std::int64_t todo_id) {
		return http::offload(pool_, [this, todo_id] -> http::Result<http::Json<Todo>> {
			auto todo = find_todo_copy(todo_id);
			if (!todo) {
				return std::unexpected{todo_not_found()};
			}
			return http::json(*todo);
		});
	}

	[[nodiscard]] http::Response mark_done(
		std::int64_t todo_id) {
		return http::offload(pool_, [this, todo_id] -> http::Result<http::Json<Todo>> {
			std::optional<Todo> todo;
			{
				std::scoped_lock lock{mutex_};
				auto it = find_todo_locked(todo_id);
				if (it == todos_.end()) {
					return std::unexpected{todo_not_found()};
				}
				it->done = true;
				todo = *it;
			}
			return http::json(*todo);
		});
	}

	[[nodiscard]] http::Response create(
		std::string title,
		std::shared_ptr<http::SseChannel> events) {
		return http::offload(
			pool_,
			[this,
			 title = std::move(title),
			 events = std::move(events)]() mutable -> http::Result<http::CreatedBody<Todo>> {
				Todo todo;
				{
					std::scoped_lock lock{mutex_};
					if (todos_.size() >= kMaxTodos) {
						return std::unexpected{http::problem::content_too_large()};
					}
					todo = Todo{.id = next_id_++, .title = std::move(title), .done = false};
					todos_.push_back(todo);
				}
				(void)events->send_event("todo.created", std::format("{}", todo.id));
				return http::created(todo).header("Location", std::format("/todos/{}", todo.id));
			});
	}

private:
	[[nodiscard]] static WorkPoolOptions state_pool_options() {
		return WorkPoolOptions{
			.threads = 2,
			.max_inject_queue = 128,
			.queue_mode = WorkPoolQueueMode::no_stealing,
			.worker_name_prefix = "cf-state",
		};
	}

	[[nodiscard]] static http::Problem todo_not_found() {
		return http::problem::not_found("todo_not_found", "todo not found");
	}

	[[nodiscard]] std::optional<Todo> find_todo_copy(
		std::int64_t todo_id) {
		std::scoped_lock lock{mutex_};
		auto it = find_todo_locked(todo_id);
		if (it == todos_.end()) {
			return std::nullopt;
		}
		return *it;
	}

	[[nodiscard]] std::vector<Todo>::iterator find_todo_locked(
		std::int64_t todo_id) {
		return std::ranges::find(todos_, todo_id, &Todo::id);
	}

	static constexpr std::size_t kMaxTodos = 1024;

	WorkPool pool_{state_pool_options()};
	std::mutex mutex_;
	std::vector<Todo> todos_;
	std::int64_t next_id_{1};
};

namespace {

volatile sig_atomic_t stop_requested = 0;

void handle_stop_signal(
	int) {
	stop_requested = 1;
}

} // namespace

int main() {
	conflux::http::MetricsRegistry metrics;
	auto app = http::app(http::Config::public_server());
	auto todos = std::make_shared<TodoService>();
	auto events = std::make_shared<http::SseChannel>(
		http::SseChannel::kDefaultMaxQueueBytes,
		conflux::http::SseOverflowPolicy::DropNewest);

	app.state_shared(todos);
	app.use(conflux::http::request_id_middleware());
	app.use(conflux::http::metrics_middleware(metrics));
	app.use(conflux::http::security_headers_middleware({
		.hsts_max_age = 0,
		.csp = "default-src 'none'; frame-ancestors 'none'",
	}));
	app.use(conflux::http::cache_control_middleware({.default_directive = "no-store"}));
	app.use(conflux::http::rate_limit_middleware({.requests = 120, .window = 60s, .burst = 20}));

	app.get("/health", [] { return http::text("ok"); });
	app.get("/", [] { return http::json(TodoList{}); });

	app.get("/todos", [](http::State<TodoService> todos) { return todos->list(); });

	app.get<"/todos/{id:i64}">(
		[](http::Path<"id", std::int64_t> id, http::State<TodoService> todos) { return todos->get(id.get()); });

	app.post<"/todos/{id:i64}/done">(
		[](http::Path<"id", std::int64_t> id, http::State<TodoService> todos) { return todos->mark_done(id.get()); });

	app.post(
		"/todos",
		[events](http::Json<CreateTodo> const &body, http::State<TodoService> todos) -> http::Result<http::Response> {
			if (body->title.empty()) {
				return std::unexpected{http::problem::bad_request("invalid_todo", "title is required")};
			}
			return todos->create(body->title, events);
		});

	app.get("/events", [events] { return http::sse(events); });

	std::vector<http::Router::Middleware> metrics_auth;
	metrics_auth.push_back(bearer_auth_middleware([](std::string_view token) { return token == "metrics-token"; }));
	app.get("/metrics", conflux::http::metrics_handler_protected(metrics, std::move(metrics_auth)));

	auto server = std::move(app).prepare_server({.port = 9105});
	if (!server) {
		std::println(std::cerr, "server setup failed: {}", server.error());
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
