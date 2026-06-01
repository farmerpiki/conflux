import conflux.crypto;

int main() {
	auto encrypted = aes_gcm_encrypt({}, {}, {}, {});
	return encrypted ? 0 : 1;
}
