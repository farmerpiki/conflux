import conflux.file_map;

int main() {
	auto fn = &blocking_map_file_readonly;
	return fn == nullptr ? 0 : 1;
}
