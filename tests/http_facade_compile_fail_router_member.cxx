import conflux.http;

int main() {
	auto app = conflux::http::app();
	(void)app.router();
	return 0;
}
