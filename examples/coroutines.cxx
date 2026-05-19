// Coroutine example — co_await async file ops inside a root::Task<T> coroutine.
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>

import conflux.file_io;
import conflux.work;
import std;
import conflux.types;

namespace root = conflux::work::root;
namespace {

constexpr std::uint64_t pack_ud(
	std::uint32_t slot,
	std::uint32_t gen) noexcept {
	return (static_cast<std::uint64_t>(gen) << 32U) | slot;
}
root::Task<std::string> read_file(
	FileReader &files,
	std::string path) {
	auto handle = co_await files.async_open(AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
	if (!handle.valid()) {
		throw std::runtime_error{std::format("open {} failed", path)};
	}
	std::array<std::byte, 256> buf{};
	auto got = co_await files.read_into(handle, 0, std::span<std::byte>{buf.data(), buf.size()});
	co_return std::string{reinterpret_cast<char const *>(buf.data()), got};
}
root::Task<void> demo(
	FileReader &files,
	std::string path) {
	auto first = co_await read_file(files, path);
	auto second = co_await read_file(files, path);
	std::println("first  read: {}", first);
	std::println("second read: {}", second);
	co_return;
}

} // namespace
int main() {
	std::string const path = "/tmp/conflux_coroutine_example.txt";
	int const seed = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (seed < 0) {
		std::println(std::cerr, "open seed failed");
		return 1;
	}
	std::string_view const text = "hello from a coroutine!\n";
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
