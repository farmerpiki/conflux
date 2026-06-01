import conflux.crypto;

int main() {
	auto decoded = base64url_decode("");
	return decoded.empty() ? 0 : 1;
}
