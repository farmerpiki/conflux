import conflux.process;

int main() {
	SpawnOptions options{};
	return options.close_other_fds ? 0 : 1;
}
