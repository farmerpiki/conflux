import conflux.work;

int main() {
	TaskSource<int> source{};
	return source.try_set_value(conflux::work::root::Success<int>{42}) ? 0 : 1;
}
