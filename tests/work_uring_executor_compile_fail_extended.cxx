import conflux.extended;

int main() {
	UringExecutorOptions options{};
	return static_cast<int>(options.ring_entries);
}
