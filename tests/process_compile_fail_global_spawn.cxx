import conflux.process;

int main() {
	auto result = spawn("/bin/true", {});
	return result ? 0 : 1;
}
