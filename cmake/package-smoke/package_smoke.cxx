import std;
import conflux.types;

int main() {
	S value{"conflux"};
	V<S> values{};
	values.emplace_back(value);

	Opt<SZ> count{values.size()};
	if (not count.has_value()) {
		return 1;
	}

	return (*count == SZ{1} and values.front() == value) ? 0 : 1;
}
