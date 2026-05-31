import conflux.net.password_hash;

auto probe() {
	return ::password_hash_argon2id_available();
}
