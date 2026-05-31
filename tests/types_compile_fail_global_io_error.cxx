import conflux.types;

int main() {
	IoError error{0, "unexpected"};
	return error.errnum();
}
