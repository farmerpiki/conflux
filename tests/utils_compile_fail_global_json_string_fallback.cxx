import conflux.utils;

int main() {
	auto value = json_string_fallback("x");
	return static_cast<int>(value.size());
}
