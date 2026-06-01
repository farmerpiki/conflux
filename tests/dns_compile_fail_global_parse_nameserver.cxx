import conflux.net.dns;

int main() {
	auto parsed = parse_nameserver("127.0.0.1");
	return parsed.has_value() ? 0 : 1;
}
