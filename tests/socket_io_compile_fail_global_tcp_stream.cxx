import conflux.socket_io.coro;

int main() {
	TcpStream stream{};
	return stream.valid() ? 0 : 1;
}
