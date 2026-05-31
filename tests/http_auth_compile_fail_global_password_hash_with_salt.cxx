import conflux.net.password_hash;

auto probe() {
	return ::password_hash_with_salt("password", "salt");
}
