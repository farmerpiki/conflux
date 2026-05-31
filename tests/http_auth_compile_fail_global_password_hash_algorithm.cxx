import conflux.net.password_hash;

auto probe() {
	return ::PasswordHashAlgorithm::pbkdf2_sha256;
}
