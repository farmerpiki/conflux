import conflux.net.jwt;

auto probe() {
	return ::jwt_sign("{}", "secret");
}
