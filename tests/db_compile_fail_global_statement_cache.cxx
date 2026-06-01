import conflux.pg;

int main() {
	StatementCache cache{};
	return cache.stable_name("SELECT 1").empty() ? 0 : 1;
}
