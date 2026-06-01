import conflux.utils;

int main() {
	auto parsed = parse_ip("127.0.0.1");
	return parsed.has_value() ? 0 : 1;
}
