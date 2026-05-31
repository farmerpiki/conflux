import conflux.net.jwt;

auto probe() {
	return ::jwt_middleware(conflux::http::JwtOptions{});
}
