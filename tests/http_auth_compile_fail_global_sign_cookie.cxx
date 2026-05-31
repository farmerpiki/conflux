import conflux.net.cookie_signing;

auto probe() {
	return ::sign_cookie("value", "secret");
}
