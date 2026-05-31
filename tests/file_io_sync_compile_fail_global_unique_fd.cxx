import conflux.file_io_sync;

int main() {
	UniqueFd fd{};
	return fd.valid() ? 0 : 1;
}
