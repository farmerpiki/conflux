import conflux.net.password_hash;

auto probe() {
	return ::pbkdf2_sha256_password_hash_options();
}
