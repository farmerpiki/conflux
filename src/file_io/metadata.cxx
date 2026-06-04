module;
#include <liburing.h>
#include <sys/stat.h>

module conflux.file_io.reader;
import std;

namespace conflux::file_io {

root::Task<void> FileReader::async_fsync(
	FileHandle const &fh,
	bool data_only) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_fsync(fd, conflux::uring::FsyncFlags{data_only ? IORING_FSYNC_DATASYNC : 0U});
	});
	auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_fallocate(
	FileHandle const &fh,
	int mode,
	std::uint64_t offset,
	std::uint64_t len) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	visit_fd(fh, [&](RingFd auto fd) { sqe.prep_fallocate(fd, static_cast<std::uint32_t>(mode), offset, len); });
	auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_fadvise(
	FileHandle const &fh,
	std::uint64_t offset,
	std::uint32_t len,
	int advice) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	visit_fd(fh, [&](RingFd auto fd) { sqe.prep_fadvise(fd, offset, len, static_cast<std::uint32_t>(advice)); });
	auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_madvise(
	void *addr,
	std::uint32_t length,
	int advice) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	sqe.prep_madvise(addr, length, static_cast<std::uint32_t>(advice));
	auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_unlink(
	int dir_fd,
	std::string path,
	int flags) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	auto path_owner = std::make_shared<std::string>(std::move(path));
	sqe.prep_unlinkat(conflux::uring::SqeFd{dir_fd}, path_owner->c_str(), flags);
	auto [slot, gen] = reserve_bridge<void>(shared_src, [path_owner](IoResult) mutable {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_rename(
	int old_dir_fd,
	std::string old_path,
	int new_dir_fd,
	std::string new_path,
	unsigned flags) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	auto paths = std::make_shared<std::pair<std::string, std::string>>(std::move(old_path), std::move(new_path));
	sqe.prep_renameat(
		conflux::uring::SqeFd{old_dir_fd},
		paths->first.c_str(),
		conflux::uring::SqeFd{new_dir_fd},
		paths->second.c_str(),
		flags);
	auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_mkdirat(
	int dir_fd,
	std::string path,
	mode_t mode) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	auto path_owner = std::make_shared<std::string>(std::move(path));
	sqe.prep_mkdirat(conflux::uring::SqeFd{dir_fd}, path_owner->c_str(), mode);
	auto [slot, gen] = reserve_bridge<void>(shared_src, [path_owner](IoResult) mutable {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_symlinkat(
	std::string target,
	int new_dir_fd,
	std::string link_path) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	auto paths = std::make_shared<std::pair<std::string, std::string>>(std::move(target), std::move(link_path));
	sqe.prep_symlinkat(paths->first.c_str(), conflux::uring::SqeFd{new_dir_fd}, paths->second.c_str());
	auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_ftruncate(
	FileHandle const &fh,
	std::uint64_t length) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	visit_fd(fh, [&](RingFd auto fd) { sqe.prep_ftruncate(fd, static_cast<std::int64_t>(length)); });
	auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_linkat(
	int old_dir_fd,
	std::string old_path,
	int new_dir_fd,
	std::string new_path,
	int flags) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	auto paths = std::make_shared<std::pair<std::string, std::string>>(std::move(old_path), std::move(new_path));
	sqe.prep_linkat(
		conflux::uring::SqeFd{old_dir_fd},
		paths->first.c_str(),
		conflux::uring::SqeFd{new_dir_fd},
		paths->second.c_str(),
		flags);
	auto [slot, gen] = reserve_bridge<void>(shared_src, [paths](IoResult) mutable {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_sync_file_range(
	FileHandle const &fh,
	std::uint64_t offset,
	unsigned len,
	int flags) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	visit_fd(fh, [&](RingFd auto fd) { sqe.prep_sync_file_range(fd, len, offset, flags); });
	auto [slot, gen] = reserve_bridge<void>(shared_src, [](IoResult) {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

} // namespace conflux::file_io
