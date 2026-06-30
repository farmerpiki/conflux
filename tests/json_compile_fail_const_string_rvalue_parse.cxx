import std;
import conflux.json;

std::string const body() {
	return "{\"name\":\"value\"}";
}

int main() {
	auto doc = conflux::json::parse(body());
	return doc.has_value() ? 0 : 1;
}
