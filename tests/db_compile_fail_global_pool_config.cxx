import conflux.pg;

int main() {
	PoolConfig config{};
	return config.max_connections == 0 ? 0 : 1;
}
