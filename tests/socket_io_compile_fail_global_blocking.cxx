import conflux.socket_io.blocking;

int main() {
	try {
		throw conflux::socket_io::SyncWaitSocketTaskTimeout{};
	} catch (SyncWaitSocketTaskTimeout const &) { return 0; }
}
