import conflux.crypto;

int main() {
	auto fn = &hmac_sha256_precomputed;
	return fn == nullptr ? 0 : 1;
}
