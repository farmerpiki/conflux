import conflux.pg;

int main() {
	PgError error{"db"};
	return error.is_deadlock() ? 0 : 1;
}
