import conflux.file_watch;

int main() {
	FileEventKind kind = FileEventKind::created;
	return kind == FileEventKind::created ? 0 : 1;
}
