import conflux.crypto;

int main() {
	auto encoded = base64url_encode({});
	return encoded.empty() ? 0 : 1;
}
