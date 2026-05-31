import conflux.file_io.buffers;

int main() {
	FixedBuffer buffer{};
	return buffer.valid() ? 0 : 1;
}
