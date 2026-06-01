import conflux.net.dns;

int main() {
	auto wire = encode_query(1, "example.com", QType::a);
	return static_cast<int>(wire.size());
}
