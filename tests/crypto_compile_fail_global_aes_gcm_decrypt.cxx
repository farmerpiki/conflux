import conflux.crypto;

int main() {
	auto decrypted = aes_gcm_decrypt({}, {}, {}, {});
	return decrypted ? 0 : 1;
}
