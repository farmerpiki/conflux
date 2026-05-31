import conflux.json;

int main() {
	NdjsonRange range{"null\n"};
	return range.begin() == range.end() ? 0 : 1;
}
