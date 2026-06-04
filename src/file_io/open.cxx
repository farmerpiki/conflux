module;
#include <cerrno>
#include <liburing.h>
#include <linux/openat2.h>
#include <unistd.h>

module conflux.file_io.reader;
import std;

namespace conflux::file_io {

bool FileReader::submit_open_direct_fallback(
	std::shared_ptr<root::TaskSource<FileHandle>> const &src,
	std::shared_ptr<std::string> const &path_owner,
	int dir_fd,
	int flags,
	mode_t mode,
	unsigned file_index) {
	auto *sqe = io_uring_get_sqe(ring_);
	if (sqe == nullptr) {
		auto _ = src->try_set_exception(std::make_exception_ptr(IoError{ENOSPC, "file_io: SQ full"}));
		return false;
	}
	conflux::uring::Sqe{sqe}.prep_openat(conflux::uring::SqeFd{dir_fd}, path_owner->c_str(), flags, mode);
	auto [slot, gen] = completions_->reserve([this, src, path_owner, file_index](IoResult r) mutable {
		try {
			if (r.res < 0) {
				auto _ = src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: open_direct"}));
				return;
			}
			int const fd = r.res;
			int const update_rc = ::io_uring_register_files_update(ring_, file_index, &fd, 1);
			::close(fd);
			if (update_rc < 0) {
				int const sparse = -1;
				{ auto _ = ::io_uring_register_files_update(ring_, file_index, &sparse, 1); }
				auto _ = src->try_set_exception(std::make_exception_ptr(IoError{-update_rc, "file_io: open_direct"}));
				return;
			}
			auto _ = src->try_set_value(
				root::Success<FileHandle>{FileHandle::from_direct_slot(static_cast<int>(file_index))});
		} catch (...) { auto _ = src->try_set_exception(std::current_exception()); }
	});
	io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
	return true;
}

root::Task<FileHandle> FileReader::async_open(
	int dir_fd,
	std::string path,
	int flags,
	mode_t mode) {
	auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
	if (!sqe) {
		return std::move(task);
	}
	auto path_owner = std::make_shared<std::string>(std::move(path));
	sqe.prep_openat(conflux::uring::SqeFd{dir_fd}, path_owner->c_str(), flags, mode);
	auto [slot, gen] = completions_->reserve([shared_src, path_owner](IoResult r) mutable {
		try {
			if (r.res < 0) {
				auto _ = shared_src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: open"}));
				return;
			}
			auto _ = shared_src->try_set_value({FileHandle::from_fd(r.res)});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<FileHandle> FileReader::async_open_direct(
	int dir_fd,
	std::string path,
	int flags,
	mode_t mode,
	unsigned file_index) {
	auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
	if (!sqe) {
		return std::move(task);
	}
	auto path_owner = std::make_shared<std::string>(std::move(path));
	sqe.prep_openat_direct(
		conflux::uring::SqeFd{dir_fd},
		path_owner->c_str(),
		flags,
		mode,
		conflux::uring::DirectSlot{file_index});
	auto [slot, gen] =
		completions_->reserve([this, shared_src, path_owner, dir_fd, flags, mode, file_index](IoResult r) mutable {
			try {
				if (r.res < 0) {
					int const err = -r.res;
					if (err == EINVAL || err == EOPNOTSUPP || err == ENOSYS) {
						auto _ = submit_open_direct_fallback(shared_src, path_owner, dir_fd, flags, mode, file_index);
						return;
					}
					auto _ =
						shared_src->try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: open_direct"}));
					return;
				}
				auto _ = shared_src->try_set_value(
					{FileHandle::from_direct_slot(r.res == 0 ? static_cast<int>(file_index) : r.res)});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<FileHandle> FileReader::async_openat2(
	int dir_fd,
	std::string path,
	open_how how) {
	auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
	if (!sqe) {
		return std::move(task);
	}
	auto ctx = std::make_shared<std::pair<std::string, open_how>>(std::move(path), how);
	sqe.prep_openat2(conflux::uring::SqeFd{dir_fd}, ctx->first.c_str(), &ctx->second);
	auto [slot, gen] =
		reserve_bridge<FileHandle>(shared_src, [ctx](IoResult r) mutable { return FileHandle::from_fd(r.res); });
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<FileHandle> FileReader::async_openat2_direct(
	int dir_fd,
	std::string path,
	open_how how,
	unsigned file_index) {
	auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
	if (!sqe) {
		return std::move(task);
	}
	auto ctx = std::make_shared<std::pair<std::string, open_how>>(std::move(path), how);
	sqe.prep_openat2_direct(
		conflux::uring::SqeFd{dir_fd},
		ctx->first.c_str(),
		&ctx->second,
		conflux::uring::DirectSlot{file_index});
	auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [ctx, file_index](IoResult) mutable {
		return FileHandle::from_direct_slot(static_cast<int>(file_index));
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<FileHandle> FileReader::async_openat_direct(
	int dir_fd,
	std::string path,
	int flags,
	mode_t mode,
	unsigned file_index) {
	auto [task, shared_src, sqe] = prepare_sqe<FileHandle>();
	if (!sqe) {
		return std::move(task);
	}
	auto p = std::make_shared<std::string>(std::move(path));
	sqe.prep_openat_direct(
		conflux::uring::SqeFd{dir_fd},
		p->c_str(),
		flags,
		mode,
		conflux::uring::DirectSlot{file_index});
	auto [slot, gen] = reserve_bridge<FileHandle>(shared_src, [p, file_index](IoResult r) mutable {
		int const s = (file_index == IORING_FILE_INDEX_ALLOC) ? r.res : static_cast<int>(file_index);
		return FileHandle::from_direct_slot(s);
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

} // namespace conflux::file_io
