import conflux.file_map;

int main() {
	auto fn = &make_mapped_file_lease;
	return fn == nullptr ? 0 : 1;
}
