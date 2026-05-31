import conflux.net.auth;

auto probe(
	conflux::http::RequestView const &request) {
	return ::auth_throttle_bearer_key(request);
}
