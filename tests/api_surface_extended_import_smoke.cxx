import conflux.extended;

int main() {
	conflux::http::Config cfg = conflux::http::Config::test();
	cfg.port = 0;

	auto req = conflux::http::ClientRequest::get("http://127.0.0.1/");
	(void)req;
	return 0;
}
