module;
#include <liburing.h>
#include <sys/uio.h>

module conflux.file_io.reader;
import std;

namespace conflux::file_io {

root::JoinTask<std::size_t> FileReader::read_into(
	FileHandle const &fh,
	std::uint64_t offset,
	std::span<std::byte> dst) {
	auto [task, src, sqe] = prepare_sqe_direct<std::size_t>();
	if (!sqe) {
		return root::require_join(std::move(task));
	}
	visit_fd(fh, [&](RingFd auto fd) { sqe.prep_read(fd, dst.data(), dst.size(), offset); });
	auto [slot, gen] =
		reserve_bridge_direct<std::size_t>(std::move(src), [](IoResult r) { return static_cast<std::size_t>(r.res); });
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return root::require_join(std::move(task));
}

root::Task<std::size_t> FileReader::readv_into(
	FileHandle const &fh,
	std::uint64_t offset,
	std::vector<iovec> iovecs) {
	auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
	if (!sqe) {
		return std::move(task);
	}
	auto iov_owner = std::make_shared<std::vector<iovec>>(std::move(iovecs));
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_readv(fd, iov_owner->data(), static_cast<unsigned>(iov_owner->size()), offset);
	});
	auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [iov_owner](IoResult r) mutable {
		return static_cast<std::size_t>(r.res);
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<FileReader::ReadFixedResult> FileReader::read_fixed(
	FileHandle const &fh,
	std::uint64_t offset,
	FixedBuffer buf,
	std::size_t max_bytes) {
	auto [task, src, sqe] = prepare_sqe_direct<ReadFixedResult>();
	if (!sqe) {
		return std::move(task);
	}
	unsigned const slot_idx = buf.slot();
	std::size_t const bytes = std::min(buf.view().size(), max_bytes);
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_read_fixed(
			fd,
			buf.view().data(),
			bytes,
			offset,
			conflux::uring::FixedBufIdx{static_cast<int>(slot_idx)});
	});
	auto [slot, gen] = completions_->reserve([src = std::move(src), buf = std::move(buf)](IoResult r) mutable {
		try {
			if (r.res < 0) {
				auto _ = src.try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: read_fixed"}));
				return;
			}
			ReadFixedResult result{.buffer = std::move(buf), .bytes = static_cast<std::size_t>(r.res)};
			auto _ = src.try_set_value(root::Success<ReadFixedResult>{std::move(result)});
		} catch (...) { auto _ = src.try_set_exception(std::current_exception()); }
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<FileReader::ReadFixedResult> FileReader::read_nocache_fixed(
	FileHandle const &fh,
	std::uint64_t offset,
	FixedBuffer buf,
	std::size_t max_bytes,
	std::size_t block_size) {
	std::size_t const actual_cap = std::min(max_bytes, buf.size());
	std::size_t aligned_bytes = actual_cap;
	if (block_size > 1 && actual_cap > 0) {
		aligned_bytes = ((actual_cap + block_size - 1) / block_size) * block_size;
		aligned_bytes = std::min(aligned_bytes, buf.size());
	}
	auto [task, src, sqe] = prepare_sqe_direct<ReadFixedResult>();
	if (!sqe) {
		return std::move(task);
	}
	unsigned const slot_idx = buf.slot();
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_read_fixed(
			fd,
			buf.view().data(),
			aligned_bytes,
			offset,
			conflux::uring::FixedBufIdx{static_cast<int>(slot_idx)});
	});
	auto [slot, gen] =
		completions_->reserve([src = std::move(src), buf = std::move(buf), actual_cap](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ =
						src.try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: read_nocache_fixed"}));
					return;
				}
				std::size_t const bytes = std::min(static_cast<std::size_t>(r.res), actual_cap);
				ReadFixedResult result{.buffer = std::move(buf), .bytes = bytes};
				auto _ = src.try_set_value(root::Success<ReadFixedResult>{std::move(result)});
			} catch (...) { auto _ = src.try_set_exception(std::current_exception()); }
		});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<FileReader::WriteFixedResult> FileReader::write_fixed(
	FileHandle const &fh,
	std::uint64_t offset,
	FixedBuffer buf,
	std::size_t max_bytes) {
	auto [task, src, sqe] = prepare_sqe_direct<WriteFixedResult>();
	if (!sqe) {
		return std::move(task);
	}
	unsigned const slot_idx = buf.slot();
	std::size_t const bytes = std::min(buf.view().size(), max_bytes);
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_write_fixed(
			fd,
			buf.view().data(),
			bytes,
			offset,
			conflux::uring::FixedBufIdx{static_cast<int>(slot_idx)});
	});
	auto [slot, gen] = completions_->reserve([src = std::move(src), buf = std::move(buf)](IoResult r) mutable {
		try {
			if (r.res < 0) {
				auto _ = src.try_set_exception(std::make_exception_ptr(IoError{-r.res, "file_io: write_fixed"}));
				return;
			}
			WriteFixedResult result{.buffer = std::move(buf), .bytes = static_cast<std::size_t>(r.res)};
			auto _ = src.try_set_value(root::Success<WriteFixedResult>{std::move(result)});
		} catch (...) { auto _ = src.try_set_exception(std::current_exception()); }
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::JoinTask<std::size_t> FileReader::write_into(
	FileHandle const &fh,
	std::uint64_t offset,
	std::span<std::byte const> src_view) {
	auto [task, src, sqe] = prepare_sqe_direct<std::size_t>();
	if (!sqe) {
		return root::require_join(std::move(task));
	}
	visit_fd(fh, [&](RingFd auto fd) { sqe.prep_write(fd, src_view.data(), src_view.size(), offset); });
	auto [slot, gen] =
		reserve_bridge_direct<std::size_t>(std::move(src), [](IoResult r) { return static_cast<std::size_t>(r.res); });
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return root::require_join(std::move(task));
}

root::Task<std::size_t> FileReader::writev_into(
	FileHandle const &fh,
	std::uint64_t offset,
	std::vector<iovec> iovecs) {
	auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
	if (!sqe) {
		return std::move(task);
	}
	auto iov_owner = std::make_shared<std::vector<iovec>>(std::move(iovecs));
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_writev(fd, iov_owner->data(), static_cast<unsigned>(iov_owner->size()), offset);
	});
	auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [iov_owner](IoResult r) mutable {
		return static_cast<std::size_t>(r.res);
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<std::size_t> FileReader::readv2_into(
	FileHandle const &fh,
	std::uint64_t offset,
	std::vector<iovec> iovecs,
	int rwf_flags) {
	auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
	if (!sqe) {
		return std::move(task);
	}
	auto iov_owner = std::make_shared<std::vector<iovec>>(std::move(iovecs));
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_readv2(fd, iov_owner->data(), static_cast<unsigned>(iov_owner->size()), offset, rwf_flags);
	});
	auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [iov_owner](IoResult r) mutable {
		return static_cast<std::size_t>(r.res);
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<std::size_t> FileReader::writev2_into(
	FileHandle const &fh,
	std::uint64_t offset,
	std::vector<iovec> iovecs,
	int rwf_flags) {
	auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
	if (!sqe) {
		return std::move(task);
	}
	auto iov_owner = std::make_shared<std::vector<iovec>>(std::move(iovecs));
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_writev2(fd, iov_owner->data(), static_cast<unsigned>(iov_owner->size()), offset, rwf_flags);
	});
	auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [iov_owner](IoResult r) mutable {
		return static_cast<std::size_t>(r.res);
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<std::size_t> FileReader::async_write_fixed(
	FileHandle const &fh,
	std::uint64_t offset,
	void const *buf,
	unsigned nbytes,
	int buf_index) {
	auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
	if (!sqe) {
		return std::move(task);
	}
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_write_fixed(fd, buf, nbytes, offset, conflux::uring::FixedBufIdx{buf_index});
	});
	auto [slot, gen] =
		reserve_bridge<std::size_t>(shared_src, [](IoResult r) { return static_cast<std::size_t>(r.res); });
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

} // namespace conflux::file_io
