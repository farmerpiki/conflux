import conflux.work.root;

int main() {
	auto [task, source] = conflux::work::root::make_task_source<int>();
	(void)source;
	auto state = task.consume_for_join();
	return state ? 0 : 1;
}
