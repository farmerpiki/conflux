import conflux.crypto;

int main() {
	auto digest = sha1({});
	return static_cast<int>(digest[0]);
}
