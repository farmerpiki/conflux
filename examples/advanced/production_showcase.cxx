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
import conflux.work;
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

struct TodoRecord {
	std::int64_t id{};
	std::string title;
	std::atomic_bool done{false};

	[[nodiscard]] Todo snapshot() const {
		return Todo{
			.id = id,
			.title = title,
			.done = done.load(std::memory_order_acquire),
		};
	}
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

class TodoService {
public:
	TodoService() = default;
	~TodoService() { pool_.drain_and_stop(); }

	TodoService(TodoService const &) = delete;
	TodoService &operator =(TodoService const &) = delete;

	[[nodiscard]] http::Response list() {
		return http::offload(pool_, [this] {
			TodoList list;
			list.items.reserve(slots_.size());
			for (auto const &slot: slots_) {
				if (slot.state.load(std::memory_order_acquire) == SlotState::full) {
					list.items.push_back(slot.todo->snapshot());
				}
			}
			return http::into_response(http::json(std::move(list)));
		});
	}

	[[nodiscard]] http::Response get(
		std::int64_t todo_id) {
		return http::offload(pool_, [this, todo_id] {
			if (todo_id <= 0 || static_cast<std::uint64_t>(todo_id) > slots_.size()) {
				return http::into_response(http::problem::not_found("todo_not_found", "todo not found"));
			}
			auto const &slot = slots_[static_cast<std::size_t>(todo_id - 1)];
			if (slot.state.load(std::memory_order_acquire) != SlotState::full) {
				return http::into_response(http::problem::not_found("todo_not_found", "todo not found"));
			}
			return http::into_response(http::json(slot.todo->snapshot()));
		});
	}

	[[nodiscard]] http::Response mark_done(
		std::int64_t todo_id) {
		return http::offload(pool_, [this, todo_id] {
			if (todo_id <= 0 || static_cast<std::uint64_t>(todo_id) > slots_.size()) {
				return http::into_response(http::problem::not_found("todo_not_found", "todo not found"));
			}
			auto const &slot = slots_[static_cast<std::size_t>(todo_id - 1)];
			if (slot.state.load(std::memory_order_acquire) != SlotState::full) {
				return http::into_response(http::problem::not_found("todo_not_found", "todo not found"));
			}
			slot.todo->done.store(true, std::memory_order_release);
			return http::into_response(http::json(slot.todo->snapshot()));
		});
	}

	[[nodiscard]] http::Response create(
		std::string title,
		std::shared_ptr<http::SseChannel> events) {
		return http::offload(pool_, [this, title = std::move(title), events = std::move(events)]() mutable {
			auto const id = next_id_.fetch_add(1, std::memory_order_relaxed);
			if (id > static_cast<std::int64_t>(slots_.size())) {
				return http::into_response(http::problem::content_too_large());
			}
			auto &slot = slots_[static_cast<std::size_t>(id - 1)];
			auto expected = SlotState::empty;
			if (!slot.state.compare_exchange_strong(expected, SlotState::writing, std::memory_order_acq_rel)) {
				return http::into_response(http::problem::internal_error("todo_slot_busy", "todo slot is busy"));
			}
			slot.todo = std::make_unique<TodoRecord>();
			slot.todo->id = id;
			slot.todo->title = std::move(title);
			slot.state.store(SlotState::full, std::memory_order_release);
			(void)events->send_event("todo.created", std::format("{}", slot.todo->id));
			return http::into_response(
				http::created(slot.todo->snapshot()).header("Location", std::format("/todos/{}", slot.todo->id)));
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

	static constexpr std::size_t kMaxTodos = 1024;
	enum class SlotState : std::uint8_t {
		empty,
		writing,
		full,
		deleted,
	};

	struct TodoSlot {
		std::atomic<SlotState> state{SlotState::empty};
		std::unique_ptr<TodoRecord> todo;
	};

	WorkPool pool_{state_pool_options()};
	std::array<TodoSlot, kMaxTodos> slots_{};
	std::atomic<std::int64_t> next_id_{1};
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
	auto todos = std::make_shared<TodoService>();
	auto events =
		std::make_shared<http::SseChannel>(http::SseChannel::kDefaultMaxQueueBytes, SseOverflowPolicy::DropNewest);

	app.state(todos);
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

	app.get("/todos", [](http::State<std::shared_ptr<TodoService>> todos) { return (*todos)->list(); });

	app.get<"/todos/{id:i64}">([](http::Path<"id", std::int64_t> id, http::State<std::shared_ptr<TodoService>> todos) {
		return (*todos)->get(id.get());
	});

	app.post<"/todos/{id:i64}/done">(
		[](http::Path<"id", std::int64_t> id, http::State<std::shared_ptr<TodoService>> todos) {
			return (*todos)->mark_done(id.get());
		});

	app.post("/todos", [events](http::Json<CreateTodo> const &body, http::State<std::shared_ptr<TodoService>> todos) {
		if (body->title.empty()) {
			return http::into_response(http::problem::bad_request("invalid_todo", "title is required"));
		}
		return (*todos)->create(body->title, events);
	});

	app.get("/events", [events] { return http::sse(events); });

	std::vector<http::Router::Middleware> metrics_auth;
	metrics_auth.push_back(bearer_auth_middleware([](std::string_view token) { return token == "metrics-token"; }));
	app.get("/metrics", metrics_handler_protected(metrics, std::move(metrics_auth)));

	auto server = std::move(app).listen({.port = 9105});
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
