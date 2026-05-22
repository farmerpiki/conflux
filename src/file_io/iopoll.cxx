module;
#include <cerrno>
#include <liburing.h>

export module conflux.file_io.iopoll;

import std;
import conflux.types;
import conflux.work;
import conflux.uring;
import conflux.uring.timeout;
export import conflux.uring.completion;
export import conflux.uring.handle;
export import conflux.file_io_sync;
export import conflux.file_io.buffers;
export import conflux.file_io.reader;

namespace root = conflux::work::root;

// ---------------------------------------------------------------------------
// IopollFileReader / IopollStorageRing: storage-only fixed-buffer reads on a
// dedicated IORING_SETUP_IOPOLL ring.
//
// IOPOLL rings reject non-storage operations; keep this surface deliberately
// narrow. Open/stat/close, sockets, poll, timeout, splice, and fsync remain on
// FileReader/general rings. This type only submits READ_FIXED-style storage
// reads against caller-owned O_DIRECT file handles and registered buffers.
// ---------------------------------------------------------------------------

export struct IopollStorageRingOptions {
	unsigned entries{64};
	unsigned fixed_buffer_slots{16};
	std::size_t fixed_buffer_bytes{std::size_t{64} * 1024};
	bool hybrid_iopoll{false};
};

export class IopollFileReader {
	io_uring *ring_{};
	CompletionTable *completions_{};
	UserDataFn encode_ud_{};

	template<typename T>
	root::Task<T> fail(
		int err,
		char const *message) const {
		auto [task, raw_src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<T>>(std::move(raw_src));
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(FileIoError{err, message}));
		return std::move(task);
	}

	[[nodiscard]] root::Task<FileReader::ReadFixedResult> ready_read_result(
		FixedBuffer buf,
		std::size_t bytes) const {
		auto [task, raw_src] =
			root::make_task_source<FileReader::ReadFixedResult>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<FileReader::ReadFixedResult>>(std::move(raw_src));
		auto _ = shared_src->try_set_value({
			FileReader::ReadFixedResult{.buffer = std::move(buf), .bytes = bytes}
        });
		return std::move(task);
	}

public:
	IopollFileReader(
		io_uring *ring,
		CompletionTable *completions,
		UserDataFn encoder)
		: ring_{ring}
		, completions_{completions}
		, encode_ud_{std::move(encoder)} {}
	IopollFileReader(IopollFileReader const &) = delete;
	IopollFileReader &operator =(IopollFileReader const &) = delete;
	IopollFileReader(IopollFileReader &&) = delete;
	IopollFileReader &operator =(IopollFileReader &&) = delete;
	~IopollFileReader() {} // NOLINT(modernize-use-equals-default) — GCC module bug

	[[nodiscard]] io_uring *ring() const noexcept { return ring_; }
	[[nodiscard]] CompletionTable *completions() const noexcept { return completions_; }
	[[nodiscard]] std::uint64_t encode_ud(
		std::uint32_t slot,
		std::uint32_t gen) const {
		return encode_ud_(slot, gen);
	}

	// Storage-only O_DIRECT read into a buffer registered on this IOPOLL ring.
	// `fh` must remain alive until the CQE fires. For the raw-fd path it should
	// be an O_DIRECT file descriptor; unsupported filesystems/devices surface as
	// EINVAL/EOPNOTSUPP from the kernel and should be negatively cached by higher
	// layers that choose this path repeatedly.
	[[nodiscard]] root::Task<FileReader::ReadFixedResult> read_nocache_fixed(
		FileHandle const &fh,
		std::uint64_t offset,
		FixedBuffer buf,
		std::size_t max_bytes = std::numeric_limits<std::size_t>::max(),
		std::size_t block_size = 4096) {
		if (!fh.valid()) {
			return fail<FileReader::ReadFixedResult>(EBADF, "file_io: iopoll invalid file handle");
		}
		if (!buf.valid()) {
			return fail<FileReader::ReadFixedResult>(EINVAL, "file_io: iopoll invalid fixed buffer");
		}
		std::size_t const actual_cap = std::min(max_bytes, buf.size());
		if (actual_cap == 0) {
			return ready_read_result(std::move(buf), 0);
		}
		std::size_t aligned_bytes = actual_cap;
		if (block_size > 1) {
			aligned_bytes = ((actual_cap + block_size - 1) / block_size) * block_size;
			aligned_bytes = std::min(aligned_bytes, buf.size());
		}
		auto [task, raw_src] =
			root::make_task_source<FileReader::ReadFixedResult>(root::SubmitOptions{.enable_cancellation = false});
		auto shared_src = std::make_shared<root::TaskSource<FileReader::ReadFixedResult>>(std::move(raw_src));
		auto *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			auto _ =
				shared_src->try_set_exception(std::make_exception_ptr(FileIoError{ENOSPC, "file_io: iopoll SQ full"}));
			return std::move(task);
		}
		unsigned const slot_idx = buf.slot();
		auto holder = std::make_shared<FixedBuffer>(std::move(buf));
		io_uring_prep_read_fixed(
			sqe,
			sqe_fd_value(fh),
			holder->view().data(),
			static_cast<unsigned>(aligned_bytes),
			offset,
			static_cast<int>(slot_idx));
		apply_sqe_fd_flags(sqe, fh);
		auto [slot, gen] = completions_->reserve([shared_src, holder, actual_cap](IoResult r) mutable {
			try {
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(FileIoError{-r.res, "file_io: iopoll read_nocache_fixed"}));
					return;
				}
				std::size_t const bytes = std::min(static_cast<std::size_t>(r.res), actual_cap);
				auto _ = shared_src->try_set_value({
					FileReader::ReadFixedResult{.buffer = std::move(*holder), .bytes = bytes}
                });
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
		io_uring_sqe_set_data64(sqe, encode_ud_(slot, gen));
		return std::move(task);
	}
};

export [[nodiscard]] conflux::uring::SetupFlags iopoll_storage_setup_flags(
	IopollStorageRingOptions const &options) noexcept {
	auto flags = conflux::uring::setup_flags::iopoll | conflux::uring::setup_flags::single_issuer;
	if (options.hybrid_iopoll) {
		flags |= conflux::uring::setup_flags::hybrid_iopoll;
	}
	return flags;
}

export class IopollStorageRing {
	io_uring ring_{};
	bool ring_valid_{false};
	CompletionTable completions_{64};
	std::unique_ptr<RegisteredBufferTable> buffer_table_{};
	std::unique_ptr<FixedBufferPool> buffers_{};
	std::unique_ptr<IopollFileReader> reader_{};
	IopollStorageRingOptions options_{};

	IopollStorageRing() = default;

public:
	IopollStorageRing(IopollStorageRing const &) = delete;
	IopollStorageRing &operator =(IopollStorageRing const &) = delete;
	IopollStorageRing(IopollStorageRing &&) = delete;
	IopollStorageRing &operator =(IopollStorageRing &&) = delete;
	~IopollStorageRing() {
		reader_.reset();
		buffers_.reset();
		buffer_table_.reset();
		if (ring_valid_) {
			io_uring_queue_exit(&ring_);
		}
	}

	[[nodiscard]] static std::expected<std::unique_ptr<IopollStorageRing>, FileIoError> create(
		IopollStorageRingOptions options = {}) {
		if (options.entries == 0) {
			return std::unexpected{
				FileIoError{EINVAL, "file_io: iopoll entries must be non-zero"}
            };
		}
		if (options.fixed_buffer_slots == 0 || options.fixed_buffer_bytes == 0) {
			return std::unexpected{
				FileIoError{EINVAL, "file_io: iopoll fixed buffers must be non-empty"}
            };
		}
		auto out = std::unique_ptr<IopollStorageRing>{new IopollStorageRing{}};
		out->options_ = options;
		io_uring_params params{};
		params.flags = iopoll_storage_setup_flags(options).raw();
		int const rc = io_uring_queue_init_params(options.entries, &out->ring_, &params);
		if (rc < 0) {
			return std::unexpected{
				FileIoError{-rc, "file_io: iopoll ring init"}
            };
		}
		out->ring_valid_ = true;
		auto table = std::make_unique<RegisteredBufferTable>(&out->ring_, options.fixed_buffer_slots);
		if (!table->ok()) {
			return std::unexpected{
				FileIoError{ENOTSUP, "file_io: iopoll fixed-buffer table unsupported"}
            };
		}
		auto buffers =
			std::make_unique<FixedBufferPool>(table.get(), 0, options.fixed_buffer_slots, options.fixed_buffer_bytes);
		if (!buffers->ok() || buffers->capacity() == 0) {
			return std::unexpected{
				FileIoError{ENOTSUP, "file_io: iopoll fixed-buffer pool init"}
            };
		}
		out->buffer_table_ = std::move(table);
		out->buffers_ = std::move(buffers);
		out->reader_ = std::make_unique<IopollFileReader>(
			&out->ring_,
			&out->completions_,
			[](std::uint32_t slot, std::uint32_t gen) noexcept {
				return (static_cast<std::uint64_t>(gen) << 32U) | slot;
			});
		return out;
	}

	[[nodiscard]] bool valid() const noexcept { return ring_valid_ && reader_ != nullptr; }
	[[nodiscard]] io_uring *ring() noexcept { return &ring_; }
	[[nodiscard]] IopollFileReader &reader() noexcept { return *reader_; }
	[[nodiscard]] IopollFileReader const &reader() const noexcept { return *reader_; }
	[[nodiscard]] CompletionTable &completions() noexcept { return completions_; }
	[[nodiscard]] FixedBufferPool *buffers() noexcept { return buffers_.get(); }
	[[nodiscard]] IopollStorageRingOptions options() const noexcept { return options_; }
	[[nodiscard]] std::optional<FixedBuffer> try_acquire_buffer() {
		if (!buffers_) {
			return std::nullopt;
		}
		return buffers_->try_acquire();
	}
};

export struct IopollUdDecoder {
	std::pair<std::uint32_t, std::uint32_t> operator ()(
		std::uint64_t ud) const noexcept {
		return {static_cast<std::uint32_t>(ud & 0xFFFFFFFFU), static_cast<std::uint32_t>(ud >> 32U)};
	}
};
export struct IopollPumpTimeout final : std::runtime_error {
	IopollPumpTimeout()
		: std::runtime_error{"conflux.file_io: iopoll pump budget exhausted"} {}
};

export template<typename Decode = IopollUdDecoder>
void pump_iopoll_until(
	IopollFileReader &reader,
	std::atomic_flag const &done,
	std::optional<std::chrono::milliseconds> budget = std::nullopt,
	Decode decode = {}) {
	auto *ring = reader.ring();
	auto *completions = reader.completions();
	auto const deadline = budget ? std::make_optional(std::chrono::steady_clock::now() + *budget) : std::nullopt;
	while (!done.test(std::memory_order_acquire)) {
		::io_uring_cqe *cqe = nullptr;
		int rc = 0;
		if (deadline) {
			__kernel_timespec ts{.tv_sec = 0, .tv_nsec = 1000000};
			rc = ::io_uring_submit_and_wait_timeout(ring, &cqe, 1, &ts, nullptr);
			if (rc == -ETIME) {
				if (std::chrono::steady_clock::now() > *deadline) {
					throw IopollPumpTimeout{};
				}
				continue;
			}
		} else {
			rc = ::io_uring_submit_and_wait(ring, 1);
			if (rc >= 0) {
				rc = ::io_uring_peek_cqe(ring, &cqe);
			}
		}
		if (rc == -EINTR || rc == -EAGAIN) {
			continue;
		}
		if (rc >= 0 && cqe == nullptr) {
			continue;
		}
		if (rc < 0 || cqe == nullptr) {
			throw std::runtime_error{std::format("conflux.file_io: iopoll submit_and_wait rc={}", rc)};
		}
		std::array<::io_uring_cqe *, 32> batch{};
		for (;;) {
			unsigned const n = ::io_uring_peek_batch_cqe(ring, batch.data(), static_cast<unsigned>(batch.size()));
			if (n == 0) {
				break;
			}
			for (unsigned i = 0; i < n; ++i) {
				auto const *c = batch[static_cast<std::size_t>(i)];
				auto [slot, gen] = decode(c->user_data);
				completions->dispatch(slot, gen, c->res, c->flags);
			}
			::io_uring_cq_advance(ring, n);
			if (done.test(std::memory_order_acquire)) {
				break;
			}
		}
	}
}

export template<typename T, typename Decode = IopollUdDecoder>
T block_on_iopoll(
	IopollFileReader &reader,
	conflux::work::root::Task<T> task,
	std::optional<std::chrono::milliseconds> budget = std::nullopt,
	Decode decode = {}) {
	using namespace conflux::work::root;
	struct Slot {
		std::atomic_flag done{};
		std::exception_ptr err{};
		[[no_unique_address]] std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>> value{};
	};
	auto slot = std::make_shared<Slot>();
	auto jh = std::make_shared<TaskJoinHandle<T>>(into_join_handle(std::move(task)));
	jh->control().set_on_ready_or_run([slot, jh]() noexcept {
		try {
			auto outcome = blocking_join(std::move(*jh));
			if (outcome.is_failure()) {
				slot->err = std::move(outcome).failure().error;
			} else if (outcome.is_cancelled()) {
				slot->err = std::make_exception_ptr(::Cancelled{});
			} else if constexpr (!std::is_void_v<T>) {
				slot->value.emplace(std::move(outcome).success().value);
			}
		} catch (...) { slot->err = std::current_exception(); }
		slot->done.test_and_set(std::memory_order_release);
	});
	pump_iopoll_until(reader, slot->done, budget, std::move(decode));
	if (slot->err) {
		std::rethrow_exception(slot->err);
	}
	if constexpr (!std::is_void_v<T>) {
		return std::move(*slot->value);
	}
}
