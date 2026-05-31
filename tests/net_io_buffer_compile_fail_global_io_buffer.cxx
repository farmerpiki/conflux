import conflux.net.io_buffer;

int main() {
	IoBuffer buffer{};
	return buffer.bytes.empty() ? 0 : 1;
}
