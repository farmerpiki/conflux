import conflux.file_io_sync;

int main() {
	conflux::file_io_sync::TemporaryFileSync tmp{};
	return tmp.fd();
}
