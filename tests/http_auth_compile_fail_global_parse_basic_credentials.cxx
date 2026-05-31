import conflux.net.auth;

auto probe() {
	return ::parse_basic_credentials("Basic dXNlcjpwYXNz");
}
