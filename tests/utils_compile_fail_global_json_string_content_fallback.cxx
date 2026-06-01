import conflux.utils;

int main() {
	auto value = json_string_content_fallback("x");
	return static_cast<int>(value.size());
}
