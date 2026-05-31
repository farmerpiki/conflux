import conflux.net.auth;

auto probe() {
	return ::auth_throttle_key("scope", "subject");
}
