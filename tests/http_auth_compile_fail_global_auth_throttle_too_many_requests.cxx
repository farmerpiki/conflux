import conflux.net.auth;

auto probe(
	conflux::http::AuthThrottleOutcome const &outcome) {
	return ::auth_throttle_too_many_requests(outcome);
}
