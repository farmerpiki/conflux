import conflux.socket_io;

int main() {
	SocketTaskRingOptions opts{};
	return opts.fd_mode == conflux::socket_io::SocketFdMode::os_fd ? 0 : 1;
}
