// Typed JSON API example: App with default native JSON request/response handling.
// Build and run: build/release-clang-libcxx/conflux_api_typed_json_example
// Try:
//   curl http://localhost:9110/api/todos
//   curl -i -X POST http://localhost:9110/api/todos
//        -H 'Content-Type: application/json' -d '{"title":"write docs"}'
import conflux.extended;
import std;

namespace http = conflux::http;

struct Todo {
	std::int64_t id{};
	std::string title;
	bool done{};
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

struct TodoList {
	std::vector<Todo> items;
};

template<>
struct conflux::json::JsonMembers<TodoList> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("items", &TodoList::items),
		};
	}
	static constexpr std::string_view type_name() { return "TodoList"; }
};

struct CreateTodo {
	std::string title;
};

template<>
struct conflux::json::JsonMembers<CreateTodo> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("title", &CreateTodo::title),
		};
	}
	static constexpr std::string_view type_name() { return "CreateTodo"; }
};

struct CreateTodoResult {
	bool ok{};
	Todo todo{};
	std::string error{};
};

template<>
struct conflux::json::JsonMembers<CreateTodoResult> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("ok", &CreateTodoResult::ok),
			conflux::json::json_member("todo", &CreateTodoResult::todo),
			conflux::json::json_member("error", &CreateTodoResult::error),
		};
	}
	static constexpr std::string_view type_name() { return "CreateTodoResult"; }
};

struct StatusReply {
	std::string status;
	std::string component;
};

template<>
struct conflux::json::JsonMembers<StatusReply> {
	static constexpr auto members() {
		return std::tuple{
			conflux::json::json_member("status", &StatusReply::status),
			conflux::json::json_member("component", &StatusReply::component),
		};
	}
	static constexpr std::string_view type_name() { return "StatusReply"; }
};

struct TodoStore {
	std::mutex mu;
	std::vector<Todo> todos{
		Todo{.id = 1,           .title = "ship v1 preview", .done = false},
		Todo{.id = 2, .title = "write typed JSON examples",  .done = true},
	};
	std::int64_t next_id = 3;
};

int main() {
	auto app = http::app();
	auto store = std::make_shared<TodoStore>();
	app.state(store);

	app.get("/api/status", [] {
		return http::Json{
			StatusReply{.status = "ok", .component = "typed-json-api"}
        };
	});

	app.get("/api/todos", [](http::State<TodoStore> store) {
		std::lock_guard const lock{store->mu};
		return http::Json{TodoList{.items = store->todos}};
	});

	app.post(
		"/api/todos",
		[](http::Json<CreateTodo> const &body,
		   http::State<TodoStore> store) -> std::expected<http::Json<CreateTodoResult>, http::Problem> {
			if (body->title.empty()) {
				return std::unexpected{http::problem::bad_request("invalid_todo", "title is required")};
			}

			std::lock_guard const lock{store->mu};
			auto todo = Todo{.id = store->next_id++, .title = body->title, .done = false};
			store->todos.push_back(todo);
			return http::Json{
				CreateTodoResult{.ok = true, .todo = std::move(todo)}
            };
		});

	auto const status = http::run(std::move(app), {.port = 9110});
	return status == http::RunStatus::stopped_normally ? 0 : 1;
}
