import conflux.net.auth;
import std;

auto probe() {
	return ::bearer_auth_middleware([](std::string_view) { return true; });
}
