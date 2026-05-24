import conflux.http;

int main() {
	auto app = conflux::http::app();
	(void)app.route_infos();
	return 0;
}
