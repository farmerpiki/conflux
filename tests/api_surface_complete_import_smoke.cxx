import std;
import conflux.complete;

int main() {
	auto direct = conflux::uring::DirectFd::from_direct(0);
	if (direct.valid()) {
		return 0;
	}
	static_assert(conflux::uring::DirectFdLike<conflux::uring::DirectFd>);

	conflux::file_io_sync::UniqueFd fd;
	if (fd.valid()) {
		return 1;
	}

	SocketTaskRingOptions socket_opts{};
	socket_opts.fd_mode = SocketFdMode::direct_if_available;

	if (!conflux::net::dns::is_valid_hostname("localhost")) {
		return 1;
	}
	auto endpoint = conflux::net::dns::try_parse_ip_literal("127.0.0.1", 80);
	if (!endpoint.has_value()) {
		return 1;
	}
	return 0;
}
