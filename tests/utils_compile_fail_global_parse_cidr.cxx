import conflux.utils;

int main() {
	auto parsed = parse_cidr("127.0.0.0/8");
	return parsed.has_value() ? 0 : 1;
}
