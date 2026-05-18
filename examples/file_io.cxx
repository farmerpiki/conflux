// Advanced runtime example: direct conflux.file_io.driver with caller-owned io_uring.
//
// Demonstrates using FileReader with a caller-owned io_uring and completion
// table. The block_on helper drives the ring until each Flow resolves.
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

import conflux.file_io.driver;
import conflux.work;
import std;
import conflux.types;
namespace {

constexpr u64 pack_ud(
	u32 slot,
	u32 gen) noexcept {
	return (static_cast<u64>(gen) << 32U) | slot;
}

} // namespace
int main() {
	std::string path = "/tmp/conflux_file_io_example.txt";
	int const seed = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (seed < 0) {
		std::println(std::cerr, "open seed file failed");
		return 1;
	}
	std::string_view text = "hello from conflux.file_io\n";
	if (::write(seed, text.data(), text.size()) != static_cast<ssize_t>(text.size())) {
		std::println(std::cerr, "seed write failed");
		::close(seed);
		return 1;
	}
	::close(seed);

	io_uring ring{};
	if (::io_uring_queue_init(64, &ring, 0) < 0) {
		std::println(std::cerr, "io_uring_queue_init failed");
		return 1;
	}

	CompletionTable completions;
	FileReader files{&ring, &completions, pack_ud};

	try {
		auto handle = block_on(files, files.async_open(AT_FDCWD, path, O_RDONLY | O_CLOEXEC));
		if (!handle.valid()) {
			std::println(std::cerr, "async_open returned invalid handle");
			::io_uring_queue_exit(&ring);
			return 1;
		}

		std::array<std::byte, 128> buf{};
		auto got = block_on(files, files.read_into(handle, 0, span<byte>{buf.data(), buf.size()}));
		std::println("read {} bytes: {}", got, std::string_view{reinterpret_cast<char const *>(buf.data()), got});
	} catch (exception const &e) {
		std::println(std::cerr, "error: {}", e.what());
		::io_uring_queue_exit(&ring);
		::unlink(path.c_str());
		return 1;
	}

	::io_uring_queue_exit(&ring);
	::unlink(path.c_str());
}
