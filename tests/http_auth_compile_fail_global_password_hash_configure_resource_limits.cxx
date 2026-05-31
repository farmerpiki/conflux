import conflux.net.password_hash;

auto probe() {
	return ::password_hash_configure_resource_limits(conflux::http::PasswordHashResourceLimits{});
}
