import conflux.net.cookie_signing;

auto probe() {
	return ::verify_cookie("value.signature", "secret");
}
