import conflux.uring.completion;

int main() {
	CompletionTable table;
	return table.has_pending_zc_notifications() ? 1 : 0;
}
