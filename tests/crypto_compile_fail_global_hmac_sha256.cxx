import conflux.crypto;

int main() {
	auto digest = hmac_sha256({}, {});
	return static_cast<int>(digest[0]);
}
