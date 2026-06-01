import conflux.pg;

int main() {
	QueryOptions options{};
	return options.deadline.has_value() ? 0 : 1;
}
