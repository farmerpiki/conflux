import conflux.net.password_hash;

auto probe() {
	return ::password_needs_rehash("encoded");
}
