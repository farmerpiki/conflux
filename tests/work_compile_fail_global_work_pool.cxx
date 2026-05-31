import conflux.work;

int main() {
	WorkPool pool{};
	return pool.stopped() ? 1 : 0;
}
