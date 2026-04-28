// Coroutine example — asio-style co_await over conflux.work Flow<T>.
//
// Any Flow<T> is directly awaitable. Define a coroutine returning Task<T>,
// co_await each async op, and let block_on() drive the io_uring until done.
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

import conflux.file_io;
import conflux.work;
import std;
import conflux.types;

namespace {

constexpr uint64_t pack_ud(
	uint32_t slot,
	uint32_t gen) noexcept {
	return (static_cast<uint64_t>(gen) << 32U) | slot;
}

Task<S> read_file(
	FileReader &files,
	S path) {
	auto handle = co_await files.open_async(AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
	if (!handle.valid()) {
		throw std::runtime_error{std::format("open {} failed", path)};
	}
	A<std::byte, 256> buf{};
	auto got = co_await files.read_into(handle, 0, std::span<std::byte>{buf.data(), buf.size()});
	co_return S{reinterpret_cast<char const *>(buf.data()), got};
}

Task<void> demo(
	FileReader &files,
	S path) {
	auto first = co_await read_file(files, path);
	auto second = co_await read_file(files, path);
	std::println("first  read: {}", first);
	std::println("second read: {}", second);
	co_return;
}

} // namespace

int main() {
	S path = "/tmp/conflux_coroutine_example.txt";
	int const seed = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (seed < 0) {
		std::println(std::cerr, "open seed failed");
		return 1;
	}
	SV text = "hello from a coroutine!\n";
	(void)::write(seed, text.data(), text.size());
	::close(seed);

	io_uring ring{};
	if (::io_uring_queue_init(64, &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}
	CompletionTable completions;
	FileReader files{&ring, &completions, pack_ud};

	try {
		block_on(files, demo(files, path));
	} catch (std::exception const &e) {
		std::println(std::cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		::unlink(path.c_str());
		return 1;
	}

	::io_uring_queue_exit(&ring);
	::unlink(path.c_str());
}
