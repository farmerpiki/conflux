import conflux.crypto;

int main() {
	auto encoded = base64_encode({});
	return encoded.empty() ? 0 : 1;
}
