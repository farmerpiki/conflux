import conflux.work;

int main() {
	Cancelled cancelled{};
	return cancelled.what()[0] == '\0' ? 0 : 1;
}
