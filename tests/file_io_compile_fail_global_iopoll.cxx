import conflux.file_io.iopoll;

int main() {
	IopollStorageRingOptions options{};
	return options.entries == 0 ? 0 : 1;
}
