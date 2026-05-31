import conflux.file_io_sync;

int main() {
	conflux::file_io_sync::FileIoSyncError err{0, "legacy"};
	return err.code().value();
}
