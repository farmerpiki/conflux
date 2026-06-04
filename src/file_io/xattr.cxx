module;
#include <liburing.h>

module conflux.file_io.reader;
import std;

namespace conflux::file_io {

root::Task<std::size_t> FileReader::async_fgetxattr(
	FileHandle const &fh,
	std::string name,
	std::span<char> buf) {
	auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
	if (!sqe) {
		return std::move(task);
	}
	auto name_owner = std::make_shared<std::string>(std::move(name));
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_fgetxattr(fd, name_owner->c_str(), buf.data(), static_cast<unsigned>(buf.size()));
	});
	auto [slot, gen] = reserve_bridge<std::size_t>(shared_src, [name_owner](IoResult r) mutable {
		return static_cast<std::size_t>(r.res);
	});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_fsetxattr(
	FileHandle const &fh,
	std::string name,
	std::string data,
	int flags) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	auto kv = std::make_shared<std::pair<std::string, std::string>>(std::move(name), std::move(data));
	visit_fd(fh, [&](RingFd auto fd) {
		sqe.prep_fsetxattr(fd, kv->first.c_str(), kv->second.c_str(), flags, static_cast<unsigned>(kv->second.size()));
	});
	auto [slot, gen] = reserve_bridge<void>(shared_src, [kv](IoResult) mutable {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<std::size_t> FileReader::async_getxattr(
	std::string path,
	std::string name,
	std::span<char> buf) {
	auto [task, shared_src, sqe] = prepare_sqe<std::size_t>();
	if (!sqe) {
		return std::move(task);
	}
	auto kp = std::make_shared<std::pair<std::string, std::string>>(std::move(path), std::move(name));
	sqe.prep_getxattr(kp->second.c_str(), buf.data(), kp->first.c_str(), static_cast<unsigned>(buf.size()));
	auto [slot, gen] =
		reserve_bridge<std::size_t>(shared_src, [kp](IoResult r) mutable { return static_cast<std::size_t>(r.res); });
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

root::Task<void> FileReader::async_setxattr(
	std::string path,
	std::string name,
	std::string data,
	int flags) {
	auto [task, shared_src, sqe] = prepare_sqe<void>();
	if (!sqe) {
		return std::move(task);
	}
	struct XattrState {
		std::string path;
		std::string name;
		std::string data;
	};
	auto st = std::make_shared<XattrState>(std::move(path), std::move(name), std::move(data));
	sqe.prep_setxattr(
		st->name.c_str(),
		st->data.c_str(),
		st->path.c_str(),
		flags,
		static_cast<unsigned>(st->data.size()));
	auto [slot, gen] = reserve_bridge<void>(shared_src, [st](IoResult) mutable {});
	io_uring_sqe_set_data64(sqe.raw(), encode_ud_(slot, gen));
	return std::move(task);
}

} // namespace conflux::file_io
