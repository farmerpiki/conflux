// Typed JSON API example: App + json::routes + NativeJsonProvider.
// Build and run: build/debug-gcc-stdcxx/conflux_api_typed_json_example
// Try:
//   curl http://localhost:9110/api/todos
//   curl -i -X POST http://localhost:9110/api/todos \
//        -H 'Content-Type: application/json' -d '{"title":"write docs"}'
import conflux.net.app;
import conflux.types;
import conflux.json;
import conflux.net.http.app_json;
import conflux.net.http.native_json;
import conflux.net.http.server_types;
import std;

namespace http = conflux::http;
namespace json = conflux::json;
using JsonProvider = conflux::json::boundary::NativeJsonProvider;

struct Todo {
	std::int64_t id{};
	std::string title;
	bool done{};
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

struct TodoList {
	std::vector<Todo> items;
};

template<>
struct JsonMembers<TodoList> {
	static constexpr auto members() {
		return std::tuple{
			json_member("items", &TodoList::items),
		};
	}
	static constexpr std::string_view type_name() { return "TodoList"; }
};

struct CreateTodo {
	std::string title;
};

template<>
struct JsonMembers<CreateTodo> {
	static constexpr auto members() {
		return std::tuple{
			json_member("title", &CreateTodo::title),
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
struct JsonMembers<CreateTodoResult> {
	static constexpr auto members() {
		return std::tuple{
			json_member("ok", &CreateTodoResult::ok),
			json_member("todo", &CreateTodoResult::todo),
			json_member("error", &CreateTodoResult::error),
		};
	}
	static constexpr std::string_view type_name() { return "CreateTodoResult"; }
};

struct StatusReply {
	std::string status;
	std::string component;
};

template<>
struct JsonMembers<StatusReply> {
	static constexpr auto members() {
		return std::tuple{
			json_member("status", &StatusReply::status),
			json_member("component", &StatusReply::component),
		};
	}
	static constexpr std::string_view type_name() { return "StatusReply"; }
};

int main() {
	auto app = http::App::default_server();
	auto api = http::json::routes<JsonProvider>(app);

	std::vector<Todo> todos{
		Todo{.id = 1, .title = "ship v1 preview", .done = false},
		Todo{.id = 2, .title = "write typed JSON examples", .done = true},
	};
	mutex todos_mu;
	std::int64_t next_id = 3;

	api.get("/api/status", [] {
		return StatusReply{.status = "ok", .component = "typed-json-api"};
	});

	api.get("/api/todos", [&todos, &todos_mu] {
		lock_guard lock{todos_mu};
		return TodoList{.items = todos};
	});

	api.post_body<CreateTodo>("/api/todos", [&todos, &todos_mu, &next_id](CreateTodo const &body) {
		if (body.title.empty()) {
			return CreateTodoResult{.ok = false, .error = std::string{"title is required"}};
		}

		lock_guard lock{todos_mu};
		auto todo = Todo{.id = next_id++, .title = body.title, .done = false};
		todos.push_back(todo);
		return CreateTodoResult{.ok = true, .todo = move(todo)};
	});

	auto const status = move(app).run({.port = 9110});
	return status == RunStatus::stopped_normally ? 0 : 1;
}
