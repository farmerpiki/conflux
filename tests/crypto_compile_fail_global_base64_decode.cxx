import conflux.crypto;

int main() {
	auto decoded = base64_decode("");
	return decoded.empty() ? 0 : 1;
}
