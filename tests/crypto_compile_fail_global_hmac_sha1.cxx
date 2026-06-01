import conflux.crypto;

int main() {
	auto digest = hmac_sha1({}, {});
	return static_cast<int>(digest[0]);
}
