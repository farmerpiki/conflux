import conflux.net.auth;
import std;

auto probe(
	conflux::http::AuthFailureLimiter limiter) {
	return ::auth_throttle_middleware(std::move(limiter), [](conflux::http::RequestView const &) {
		return std::string{"subject"};
	});
}
