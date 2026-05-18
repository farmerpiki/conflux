import std;
import conflux.types;

int main() {
	std::string value{"conflux"};
	std::vector<std::string> values{};
	values.emplace_back(value);

	std::optional<std::size_t> count{values.size()};
	if (not count.has_value()) {
		return 1;
	}

	return (*count == std::size_t{1} and values.front() == value) ? 0 : 1;
}
