import conflux.net.dns;

int main() {
	DnsError error{DnsErrorKind::network, "network"};
	return error.os_errno;
}
