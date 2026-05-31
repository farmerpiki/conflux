import conflux.work;

int main() {
	RingLaneOptions options{};
	return options.queue_depth == 0 ? 0 : 1;
}
