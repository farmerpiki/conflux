import conflux.net.http.static_async;

int main() {
	auto *handler = &handle_static_get_request;
	return handler == nullptr ? 1 : 0;
}
