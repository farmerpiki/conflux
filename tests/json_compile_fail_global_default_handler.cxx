import conflux.json;

int main() {
	JsonDefaultHandler handler{};
	return parse_sax("null", handler).has_value() ? 0 : 1;
}
