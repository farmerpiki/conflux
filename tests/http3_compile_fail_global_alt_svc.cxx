import conflux.net.http3;

int main() {
	auto const value = http3_alt_svc_value(443, 86400);
	return value.empty() ? 1 : 0;
}
