import conflux.file_io.reader;

int main() {
	conflux::file_io::FileIoError err{0, "legacy"};
	return err.code().value();
}
