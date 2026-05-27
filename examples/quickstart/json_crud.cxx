import conflux;
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
	// Toy in-memory storage for the quickstart. Real services should use an
	// async database or explicitly offload blocking storage work.
	std::mutex mu;
	std::vector<Todo> todos;
	std::int64_t next_id{1};
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

int main() {
	auto app = http::app();
	auto store = std::make_shared<TodoStore>();
	app.state(store);

	app.get("/todos", [](http::State<TodoStore> store) {
		std::lock_guard lock{store->mu};
		return http::json(TodoList{.items = store->todos});
	});

	app.get<"/todos/{id:i64}">([](std::int64_t id, http::State<TodoStore> store) -> http::Result<http::Json<Todo>> {
		std::lock_guard lock{store->mu};
		auto it = std::ranges::find(store->todos, id, &Todo::id);
		if (it == store->todos.end()) {
			return std::unexpected{http::problem::not_found("todo_not_found", "todo not found")};
		}
		return http::json(*it);
	});

	app.post(
		"/todos",
		[](http::Json<CreateTodo> const &body, http::State<TodoStore> store) -> http::Result<http::CreatedBody<Todo>> {
			if (body->title.empty()) {
				return std::unexpected{http::problem::bad_request("invalid_todo", "title is required")};
			}
			std::lock_guard lock{store->mu};
			auto todo = Todo{.id = store->next_id++, .title = body->title};
			store->todos.push_back(todo);
			return http::created(todo).header("Location", std::format("/todos/{}", todo.id));
		});

	return http::exit_code(http::run(std::move(app), {.port = 9110}));
}
