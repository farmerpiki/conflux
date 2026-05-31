import conflux.file_io.pipe_pool;

int main() {
	PipePool pool{1};
	return pool.capacity() == 1 ? 0 : 1;
}
