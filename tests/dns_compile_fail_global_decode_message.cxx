import conflux.net.dns;

int main() {
	auto message = decode_message({});
	return static_cast<int>(message.answers.size());
}
