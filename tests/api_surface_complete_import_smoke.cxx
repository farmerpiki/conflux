import conflux.complete;

int main() {
	auto direct = conflux::uring::DirectFd::from_direct(0);
	if (direct.valid()) {
		return 0;
	}
	return 0;
}
