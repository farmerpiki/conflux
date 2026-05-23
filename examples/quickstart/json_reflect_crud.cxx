// Reflection JSON CRUD quickstart: zero-boilerplate struct serde.
// Requires GCC 16+ with -freflection and CONFLUX_JSON_REFLECT=ON.
import conflux;
import conflux.json.reflect;
import std;

namespace http = conflux::http;

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

int main() {
	auto app = http::app();
	auto store = std::make_shared<TodoStore>();
	app.state(store);

	app.get("/todos", [](http::State<TodoStore> store) {
		std::lock_guard lock{store->mu};
		return http::json(TodoList{.items = store->todos});
	});

	app.get<"/todos/{id:i64}">(
		[](std::int64_t id, http::State<TodoStore> store) -> std::expected<http::Json<Todo>, http::Problem> {
			std::lock_guard lock{store->mu};
			auto it = std::ranges::find(store->todos, id, &Todo::id);
			if (it == store->todos.end()) {
				return std::unexpected{http::problem::not_found("todo_not_found", "todo not found")};
			}
			return http::json(*it);
		});

	app.post(
		"/todos",
		[](http::Json<CreateTodo> const &body,
		   http::State<TodoStore> store) -> std::expected<http::Created, http::Problem> {
			if (body->title.empty()) {
				return std::unexpected{http::problem::bad_request("invalid_todo", "title is required")};
			}
			std::lock_guard lock{store->mu};
			auto todo = Todo{.id = store->next_id++, .title = body->title};
			store->todos.push_back(todo);
			return http::created(todo).header("Location", std::format("/todos/{}", todo.id));
		});

	return static_cast<int>(std::move(app).run({.port = 9118}));
}
