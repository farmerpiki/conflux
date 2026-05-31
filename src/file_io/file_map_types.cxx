module;
#include <sys/mman.h>

export module conflux.file_map.types;

import std;

namespace conflux::file_map {

struct MappedRegionEntry {
	void const *ptr{};
	std::uint64_t mmap_size{};
	std::uint64_t file_size{};
	~MappedRegionEntry() noexcept {
		if (ptr != nullptr) {
			::munmap(const_cast<void *>(ptr), static_cast<std::size_t>(mmap_size));
		}
	}
	MappedRegionEntry() noexcept = default;
	MappedRegionEntry(
		void const *p,
		std::uint64_t msz,
		std::uint64_t fsz) noexcept
		: ptr{p}
		, mmap_size{msz}
		, file_size{fsz} {}
	MappedRegionEntry(MappedRegionEntry const &) = delete;
	MappedRegionEntry &operator =(MappedRegionEntry const &) = delete;
	MappedRegionEntry(MappedRegionEntry &&) = delete;
	MappedRegionEntry &operator =(MappedRegionEntry &&) = delete;
};
export class MappedFileLease {
	std::shared_ptr<MappedRegionEntry> region_{};

public:
	MappedFileLease() noexcept = default;
	explicit MappedFileLease(
		std::shared_ptr<MappedRegionEntry> r) noexcept
		: region_{std::move(r)} {}
	[[nodiscard]] std::span<std::byte const> bytes() const noexcept {
		if (!region_ || region_->ptr == nullptr) {
			return {};
		}
		return {static_cast<std::byte const *>(region_->ptr), static_cast<std::size_t>(region_->file_size)};
	}
	[[nodiscard]] bool empty() const noexcept { return !region_ || region_->ptr == nullptr || region_->file_size == 0; }
	[[nodiscard]] std::uint64_t size() const noexcept { return region_ ? region_->file_size : 0; }
};
export struct MappedBody {
	MappedFileLease lease;
	std::uint64_t offset{};
	std::uint64_t size{};
	[[nodiscard]] std::span<std::byte const> window() const noexcept {
		auto const full = lease.bytes();
		if (offset >= full.size()) {
			return {};
		}
		auto const avail = full.size() - static_cast<std::size_t>(offset);
		auto const len = std::min(static_cast<std::size_t>(size), avail);
		return full.subspan(static_cast<std::size_t>(offset), len);
	}
};

export [[nodiscard]] MappedFileLease make_mapped_file_lease(
	void const *ptr,
	std::uint64_t mmap_size,
	std::uint64_t file_size) {
	return MappedFileLease{std::make_shared<MappedRegionEntry>(ptr, mmap_size, file_size)};
}

} // namespace conflux::file_map
