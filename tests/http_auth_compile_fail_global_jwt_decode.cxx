import conflux.net.jwt;

auto probe() {
	return ::jwt_decode("token", conflux::http::JwtOptions{});
}
