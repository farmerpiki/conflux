import conflux.net.auth;
import std;

auto probe() {
	return ::basic_auth_middleware([](std::string_view, std::string_view) {
		return true;
	});
}
