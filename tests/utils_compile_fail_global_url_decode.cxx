import conflux.utils;

int main() {
	auto decoded = url_decode("a%20b");
	return decoded.empty() ? 1 : 0;
}
