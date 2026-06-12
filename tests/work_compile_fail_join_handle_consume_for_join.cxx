import std;
import conflux.work.root;

int main() {
	auto [task, source] = conflux::work::root::make_task_source<int>();
	(void)source;
	auto handle = conflux::work::root::into_join_handle(std::move(task));
	auto state = handle.consume_for_join();
	return state ? 0 : 1;
}
