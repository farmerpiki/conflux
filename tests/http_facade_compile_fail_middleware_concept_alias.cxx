import conflux.http;

int main() {
	auto const value = conflux::http::Middleware<decltype([] {})>;
	(void)value;
}
