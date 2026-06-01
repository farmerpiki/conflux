import conflux.pg;

int main() {
	ConnectParams params{};
	return params.conninfo.empty() ? 0 : 1;
}
