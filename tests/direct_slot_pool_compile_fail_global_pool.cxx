import conflux.net.detail.direct_slot_pool;

int main() {
	DirectSlotPool pool{1};
	return static_cast<int>(pool.capacity());
}
