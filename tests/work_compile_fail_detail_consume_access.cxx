import conflux.work.root;

int main() {
	auto [task, source] = conflux::work::root::make_task_source<int>();
	(void)source;
	auto state = conflux::work::root::detail::consume_access::for_join(task);
	return state ? 0 : 1;
}
