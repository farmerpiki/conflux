import conflux.pg;

int main() {
	QueryCache cache{"."};
	return cache.lookup("query") == nullptr ? 0 : 1;
}
