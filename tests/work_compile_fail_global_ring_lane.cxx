import conflux.work;

int main() {
	RingLane lane{};
	return lane.stopped() ? 0 : 1;
}
