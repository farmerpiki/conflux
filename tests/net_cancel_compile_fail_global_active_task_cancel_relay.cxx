import conflux.net.cancel;

int main() {
	ActiveTaskCancelRelay relay;
	relay.cancel();
	return relay.is_cancelled() ? 0 : 1;
}
