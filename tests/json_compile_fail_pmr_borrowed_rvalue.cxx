import std;
import conflux.json;

int main() {
	std::pmr::monotonic_buffer_resource resource;
	auto doc = conflux::json::parse_borrowed(std::string{R"({"x":1})"}, {}, &resource);
	return doc.has_value() ? 0 : 1;
}
