import conflux.net.dns;

int main() {
	auto *resolver = current_resolver();
	return resolver == nullptr ? 0 : 1;
}
