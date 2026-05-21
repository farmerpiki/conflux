module;
#include <liburing.h>

export module conflux.uring.timeout;

import std;
import conflux.types;
import conflux.work;
import conflux.uring.completion;

namespace conflux::uring {
namespace root = conflux::work::root;

export struct UringTimeoutError final : std::runtime_error {
	explicit UringTimeoutError(
		std::string_view msg)
		: std::runtime_error{std::string{msg}} {}
};

namespace detail {

[[nodiscard]] root::Task<void> submit_timeout_(
	io_uring *ring,
	CompletionTable &completions,
	std::function<std::uint64_t(std::uint32_t, std::uint32_t)> encode_ud,
	std::chrono::milliseconds ms,
	unsigned count,
	unsigned flags,
	bool link_only) {
	auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<void>>(std::move(raw_src));
	auto *sqe = io_uring_get_sqe(ring);
	if (sqe == nullptr) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(UringTimeoutError{"uring.timeout: SQ full"}));
		return std::move(task);
	}
	auto ts = std::make_shared<__kernel_timespec>();
	auto const sec = std::chrono::duration_cast<std::chrono::seconds>(ms);
	ts->tv_sec = sec.count();
	ts->tv_nsec = (ms - sec).count() * 1000000LL;
	if (link_only) {
		io_uring_prep_link_timeout(sqe, ts.get(), flags);
	} else {
		io_uring_prep_timeout(sqe, ts.get(), count, flags);
	}
	auto [slot, gen] = completions.reserve([shared_src, ts, link_only](IoResult r) mutable {
		try {
			if (!link_only) {
				if (r.res < 0 && r.res != -ETIME && r.res != -ECANCELED) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(UringTimeoutError{"uring.timeout: timeout"}));
					return;
				}
			} else {
				if (r.res < 0 && r.res != -ETIME && r.res != -ECANCELED && r.res != -ENOENT) {
					auto _ = shared_src->try_set_exception(
						std::make_exception_ptr(UringTimeoutError{"uring.timeout: link_timeout"}));
					return;
				}
			}
			auto _ = shared_src->try_set_value(root::Success<void>{});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		auto _ = ts;
	});
	io_uring_sqe_set_data64(sqe, encode_ud(slot, gen));
	return std::move(task);
}

} // namespace detail

export [[nodiscard]] root::Task<void> async_timeout(
	io_uring *ring,
	CompletionTable &completions,
	std::function<std::uint64_t(std::uint32_t, std::uint32_t)> encode_ud,
	std::chrono::milliseconds ms,
	unsigned count = 0,
	unsigned flags = 0) {
	return detail::submit_timeout_(ring, completions, std::move(encode_ud), ms, count, flags, false);
}

export [[nodiscard]] root::Task<void> timeout_async(
	io_uring *ring,
	CompletionTable &completions,
	std::function<std::uint64_t(std::uint32_t, std::uint32_t)> encode_ud,
	std::chrono::milliseconds ms,
	unsigned count = 0,
	unsigned flags = 0) {
	return async_timeout(ring, completions, std::move(encode_ud), ms, count, flags);
}

export [[nodiscard]] root::Task<void> async_timeout_remove(
	io_uring *ring,
	CompletionTable &completions,
	std::function<std::uint64_t(std::uint32_t, std::uint32_t)> encode_ud,
	std::uint64_t user_data,
	unsigned flags = 0) {
	auto [task, raw_src] = root::make_task_source<void>(root::SubmitOptions{.enable_cancellation = false});
	auto shared_src = std::make_shared<root::TaskSource<void>>(std::move(raw_src));
	auto *sqe = io_uring_get_sqe(ring);
	if (sqe == nullptr) {
		auto _ = shared_src->try_set_exception(std::make_exception_ptr(UringTimeoutError{"uring.timeout: SQ full"}));
		return std::move(task);
	}
	io_uring_prep_timeout_remove(sqe, user_data, flags);
	auto [slot, gen] = completions.reserve([shared_src](IoResult r) mutable {
		try {
			if (r.res < 0 && r.res != -ENOENT && r.res != -EALREADY) {
				auto _ = shared_src->try_set_exception(
					std::make_exception_ptr(UringTimeoutError{"uring.timeout: timeout_remove"}));
				return;
			}
			auto _ = shared_src->try_set_value(root::Success<void>{});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	});
	io_uring_sqe_set_data64(sqe, encode_ud(slot, gen));
	return std::move(task);
}

export [[nodiscard]] root::Task<void> timeout_remove_async(
	io_uring *ring,
	CompletionTable &completions,
	std::function<std::uint64_t(std::uint32_t, std::uint32_t)> encode_ud,
	std::uint64_t user_data,
	unsigned flags = 0) {
	return async_timeout_remove(ring, completions, std::move(encode_ud), user_data, flags);
}

export [[nodiscard]] root::Task<void> async_link_timeout(
	io_uring *ring,
	CompletionTable &completions,
	std::function<std::uint64_t(std::uint32_t, std::uint32_t)> encode_ud,
	std::chrono::milliseconds ms,
	unsigned flags = 0) {
	return detail::submit_timeout_(ring, completions, std::move(encode_ud), ms, 0, flags, true);
}

export [[nodiscard]] root::Task<void> link_timeout_async(
	io_uring *ring,
	CompletionTable &completions,
	std::function<std::uint64_t(std::uint32_t, std::uint32_t)> encode_ud,
	std::chrono::milliseconds ms,
	unsigned flags = 0) {
	return async_link_timeout(ring, completions, std::move(encode_ud), ms, flags);
}

} // namespace conflux::uring
