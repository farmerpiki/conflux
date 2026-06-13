import std;
import conflux.json;

int main() {
	conflux::json::JsonArena arena;
	auto doc = arena.parse_borrowed_into(std::string{R"({"x":1})"});
	return doc.has_value() ? 0 : 1;
}
