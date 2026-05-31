import conflux.process;

int main() {
	auto result = run("/bin/true", {});
	return result ? 0 : 1;
}
