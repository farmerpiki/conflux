import std;
import conflux.types;
import conflux.json;

using namespace conflux::json;
using std::println;

static S display_path(
	JsonError const &e) {
	if (!e.path.empty()) {
		return e.path.to_pointer();
	}
	if (e.member_name) {
		return S{"/"} + *e.member_name;
	}
	return "(root)";
}

static void print_json_error(
	SV context,
	JsonError const &e) {
	S const path = display_path(e);
	if (e.source) {
		std::println(
			"{}: {} at {} (line {}, column {}, byte {})",
			context,
			e.message,
			path,
			e.source->line,
			e.source->column,
			e.source->offset);
		return;
	}
	std::println("{}: {} at {}", context, e.message, path);
}

static expected<S, JsonError> first_role_for_second_user(
	SV input) {
	JsonParseOptions opts{
		.duplicate_key = DuplicateKeyPolicy::reject,
		.warm_threshold = 4u,
	};
	auto doc = parse_copy(input, opts);
	if (!doc) {
		return unexpected(move(doc).error());
	}

	auto path = JsonPath::from_pointer("/users/1/roles/0");
	if (!path) {
		return unexpected(move(path).error());
	}

	auto role = doc->root().at(*path).and_then([](NodeRef node) {
		return node.as_string();
	});
	if (!role) {
		return unexpected(move(role).error());
	}

	// The source document is local to this helper, so detach the returned view.
	return S{*role};
}

static void example_pointer_lookup() {
	std::println("--- JSON Pointer boundary lookup ---");
	constexpr SV input = R"({
		"users": [
			{"id": 1, "roles": ["admin", "ops"]},
			{"id": 2, "roles": ["editor", "reviewer"]}
		]
	})";

	auto role = first_role_for_second_user(input);
	if (!role) {
		print_json_error("lookup failed", role.error());
		return;
	}
	std::println("second user first role: {}", *role);
}

static void example_duplicate_policy() {
	std::println("\n--- duplicate key policy ---");
	constexpr SV input = R"({"id":1,"id":2})";

	if (auto strict = parse_view(input); !strict) {
		print_json_error("strict duplicate check", strict.error());
	}

	JsonParseOptions permissive{.duplicate_key = DuplicateKeyPolicy::last_wins};
	auto doc = parse_view(input, permissive);
	if (!doc) {
		print_json_error("last-wins parse", doc.error());
		return;
	}

	auto obj = *doc->root().as_object();
	std::println("last-wins id={}", *obj.member("id")->as_i64());
}

static void example_limits() {
	std::println("\n--- parse limits ---");
	JsonParseOptions limited{.max_depth = LimitOption::bound(2)};
	auto doc = parse_view(R"({"a":{"b":{"c":1}}})", limited);
	if (!doc) {
		print_json_error("depth-limited parse", doc.error());
	}
}

int main() {
	example_pointer_lookup();
	example_duplicate_policy();
	example_limits();
}
