import conflux.crypto;

int main() {
	auto digest = sha256({});
	return static_cast<int>(digest[0]);
}
