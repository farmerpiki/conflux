import conflux.file_io.reader;

int main() {
	FileReader reader{};
	return reader.ring() == nullptr ? 0 : 1;
}
