module;

struct io_uring;

module conflux.socket_io;
import std;
import conflux.uring;

namespace conflux::socket_io {

DirectFdTable::DirectFdTable(
	io_uring *ring,
	std::uint32_t max_slots)
	: DirectFdTable(conflux::uring::RingRef{ring}, max_slots) {}

DirectFdTable::DirectFdTable(
	conflux::uring::RingRef ring,
	std::uint32_t max_slots)
	: ring_{ring}
	, capacity_{max_slots} {
	err_ = ring_.register_files_sparse(capacity_);
	if (err_ == 0) {
		registered_ = true;
	}
}

DirectFdTable::~DirectFdTable() {
	if (registered_) {
		auto _ = ring_.unregister_files();
	}
}

bool DirectFdTable::install(
	std::uint32_t slot,
	int fd) {
	if (!registered_ || slot >= capacity_) {
		return false;
	}
	return ring_.register_files_update(slot, std::span<int const>{&fd, 1}) == 1;
}

} // namespace conflux::socket_io
