module;
#include <cerrno>
#include <cstddef>
#include <cstdint>

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

module conflux.socket_io.coro;

import std;
import conflux.types;
import conflux.uring.completion;
import conflux.uring.handle;
import conflux.socket_io;
import conflux.work;

namespace wroot = conflux::work::root;

namespace conflux::socket_io {

using conflux::uring::DirectFd;
using conflux::uring::IoResult;
using conflux::uring::OsFd;
using conflux::uring::RingFd;

// ─── helpers ──────────────────────────────────────────────────────────────────

// Buffer lifetime contract for Task methods:
//   *_borrowed — caller storage is passed to the kernel. It must remain valid
//                until the operation reaches its CQE-backed terminal completion.
//                In normal awaited use, this means until co_await returns.
//                Do not destroy/drop/cancel a borrowed task unless the borrowed
//                storage outlives the underlying io_uring operation.
//   *_copy     — implementation copies input before submission; caller may drop
//                or mutate the source buffer after the call returns.
//   *_owned    — implementation takes ownership by move; no source lifetime
//                obligation remains after the call returns.

// ─── StopCause / RecvTimeoutState ────────────────────────────────────────────

enum class StopCause : std::uint8_t {
	none,
	user_cancel,
	timeout,
};
struct IoTimeoutState {
	std::atomic<StopCause> stop_cause{StopCause::none};
	std::atomic<wroot::CancelReason> cancel_reason{wroot::CancelReason::requested};
	void mark_stop(
		StopCause cause) noexcept {
		StopCause expected = StopCause::none;
		stop_cause.compare_exchange_strong(expected, cause, std::memory_order_acq_rel, std::memory_order_acquire);
	}
	void mark_cancel(
		wroot::CancelReason reason) noexcept {
		cancel_reason.store(reason, std::memory_order_release);
		mark_stop(StopCause::user_cancel);
	}
	[[nodiscard]] wroot::CancelReason reason() const noexcept { return cancel_reason.load(std::memory_order_acquire); }
};
using RecvTimeoutState = IoTimeoutState;
struct CloseState {
	std::atomic_bool cancel_requested{false};
	std::atomic<wroot::CancelReason> cancel_reason{wroot::CancelReason::requested};
};

[[nodiscard]] std::shared_ptr<__kernel_timespec> make_kernel_timespec(
	std::chrono::milliseconds timeout) {
	auto ts = std::make_shared<__kernel_timespec>();
	auto const sec = std::chrono::duration_cast<std::chrono::seconds>(timeout);
	ts->tv_sec = sec.count();
	ts->tv_nsec = (timeout - sec).count() * 1000000LL;
	return ts;
}

template<class T>
void complete_cancel_fallback(
	std::weak_ptr<wroot::TaskSource<T>> const &weak_src,
	wroot::CancelReason reason) noexcept {
	if (auto src = weak_src.lock()) {
		auto _ = src->try_set_cancelled(reason);
	}
}

template<class T>
void submit_cancel_for_ud(
	SocketTaskRing *ring_ptr,
	std::uint64_t user_data,
	std::weak_ptr<wroot::TaskSource<T>> weak_src,
	wroot::CancelReason reason) noexcept {
	auto weak_for_owner = weak_src;
	if (!ring_ptr->submit_on_owner(
			[user_data, weak_src = std::move(weak_for_owner), reason](SocketTaskRing &ring) noexcept {
				auto [slot, gen] = ring.completions().reserve([](IoResult) noexcept {});
				std::uint64_t const cancel_ud = ring.encode(slot, gen);
				if (!submit_cancel_by_ud(ring.raw(), user_data, cancel_ud)) {
					ring.completions().dispatch(slot, gen, -EBUSY, conflux::uring::CqeFlags{});
					complete_cancel_fallback(weak_src, reason);
					return;
				}
				auto _ = ring.raw().submit();
			})) {
		complete_cancel_fallback(weak_src, reason);
	}
}

// ─── TcpStreamState ───────────────────────────────────────────────────────────

struct TcpStreamState {
	SocketTaskRing *ring{};
	OwnedSocketHandle handle{};
	std::atomic_bool closing{false};
	TcpStreamState(
		SocketTaskRing *r,
		OwnedSocketHandle h) noexcept
		: ring{r}
		, handle{std::move(h)} {}
};

template<class Submit>
[[nodiscard]] wroot::Task<std::size_t> submit_cancellable_size_io(
	TcpStreamState &st,
	std::shared_ptr<void> keeper,
	char const *error_context,
	Submit &&submit) {
	auto task_src = wroot::make_shared_task_source<std::size_t>(wroot::SubmitOptions{.enable_cancellation = true});
	auto task = std::move(task_src.first);
	auto shared_src = std::move(task_src.second);
	auto cancel_reason = std::make_shared<std::atomic<wroot::CancelReason>>(wroot::CancelReason::requested);
	auto [slot, gen] = st.ring->completions().reserve(
		[shared_src, cancel_reason, keeper = std::move(keeper), error_context](IoResult r) mutable {
			try {
				if (r.res == -ECANCELED) {
					auto _ = shared_src->try_set_cancelled(cancel_reason->load(std::memory_order_acquire));
					return;
				}
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{-r.res, error_context}));
					return;
				}
				auto _ = shared_src->try_set_value(wroot::Success<std::size_t>{static_cast<std::size_t>(r.res)});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
	std::uint64_t const ud = st.ring->encode(slot, gen);
	if (!std::invoke(std::forward<Submit>(submit), ud)) {
		st.ring->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
		return task;
	}
	auto ring_ptr = st.ring;
	auto weak_src = std::weak_ptr<wroot::TaskSource<std::size_t>>{shared_src};
	auto _ = shared_src->install_cancel_hook(
		[ring_ptr, ud, weak_src = std::move(weak_src), cancel_reason](wroot::CancelReason reason) noexcept {
			cancel_reason->store(reason, std::memory_order_release);
			submit_cancel_for_ud(ring_ptr, ud, weak_src, reason);
		});
	return task;
}

template<class Submit>
[[nodiscard]] wroot::Task<std::size_t> submit_cancellable_timeout_size_io(
	TcpStreamState &st,
	std::shared_ptr<void> keeper,
	std::chrono::milliseconds timeout,
	char const *error_context,
	char const *timeout_context,
	Submit &&submit) {
	auto task_src = wroot::make_shared_task_source<std::size_t>(wroot::SubmitOptions{.enable_cancellation = true});
	auto task = std::move(task_src.first);
	auto shared_src = std::move(task_src.second);
	auto ts = make_kernel_timespec(timeout);
	auto state = std::make_shared<IoTimeoutState>();
	auto [slot, gen] = st.ring->completions().reserve(
		[shared_src, state, keeper = std::move(keeper), error_context, timeout_context](IoResult r) mutable {
			try {
				if (r.res == -ECANCELED) {
					auto cause = state->stop_cause.load(std::memory_order_acquire);
					if (cause == StopCause::user_cancel) {
						auto _ = shared_src->try_set_cancelled(state->reason());
					} else {
						auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{ETIMEDOUT, timeout_context}));
					}
					return;
				}
				if (r.res < 0) {
					auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{-r.res, error_context}));
					return;
				}
				auto _ = shared_src->try_set_value(wroot::Success<std::size_t>{static_cast<std::size_t>(r.res)});
			} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
		});
	std::uint64_t const ud = st.ring->encode(slot, gen);
	auto [tslot, tgen] = st.ring->completions().reserve([ts, state](IoResult r) mutable {
		if (r.res == -ETIME) {
			state->mark_stop(StopCause::timeout);
		}
		auto _ = ts;
	});
	std::uint64_t const timeout_ud = st.ring->encode(tslot, tgen);
	if (!std::invoke(std::forward<Submit>(submit), ud, timeout_ud, ts.get())) {
		st.ring->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
		st.ring->completions().dispatch(tslot, tgen, -EBUSY, conflux::uring::CqeFlags{});
		return task;
	}
	auto ring_ptr = st.ring;
	auto weak_src = std::weak_ptr<wroot::TaskSource<std::size_t>>{shared_src};
	auto _ = shared_src->install_cancel_hook([ring_ptr, ud, weak_src, state](wroot::CancelReason reason) noexcept {
		state->mark_cancel(reason);
		submit_cancel_for_ud(ring_ptr, ud, weak_src, state->reason());
	});
	return task;
}
// ─── TcpStream ───────────────────────────────────────────────────────────────

[[nodiscard]] TcpStreamState &tcp_state(
	std::shared_ptr<void> const &state) noexcept {
	return *static_cast<TcpStreamState *>(state.get());
}

TcpStream::TcpStream() noexcept = default;
TcpStream::TcpStream(
	std::shared_ptr<void> state) noexcept
	: state_{std::move(state)} {}
TcpStream::~TcpStream() = default;
TcpStream::TcpStream(TcpStream &&) noexcept = default;
TcpStream &TcpStream::operator =(TcpStream &&) noexcept = default;

[[nodiscard]] wroot::Task<std::size_t> TcpStream::do_send(
	std::uint8_t const *data,
	std::size_t len,
	std::shared_ptr<void> keeper) {
	auto &st = tcp_state(state_);
	if (!st.handle.valid() || st.closing.load(std::memory_order_relaxed)) {
		return wroot::make_error_task<std::size_t>(IoError{EBADF, "tcp: stream closed"});
	}
	OsFd const h = st.handle.get();
	return submit_cancellable_size_io(
		st,
		std::move(keeper),
		"tcp: send",
		[ring = st.ring, h, data, len](std::uint64_t ud) {
			return submit_send_borrowed(ring->raw(), h, data, len, ud);
		});
}

[[nodiscard]] bool TcpStream::valid() const noexcept {
	return state_ && tcp_state(state_).handle.valid() && !tcp_state(state_).closing.load(std::memory_order_relaxed);
}

[[nodiscard]] int TcpStream::raw_fd() const noexcept {
	return state_ ? tcp_state(state_).handle.raw_fd() : -1;
}

[[nodiscard]] wroot::Task<std::size_t> TcpStream::async_recv_borrowed(
	std::span<std::uint8_t> dst) {
	auto &st = tcp_state(state_);
	if (!st.handle.valid() || st.closing.load(std::memory_order_relaxed)) {
		return wroot::make_error_task<std::size_t>(IoError{EBADF, "tcp: stream closed"});
	}
	OsFd const h = st.handle.get();
	return submit_cancellable_size_io(
		st,
		{},
		"tcp: recv",
		[ring = st.ring, h, data = dst.data(), size = dst.size()](std::uint64_t ud) {
			return submit_async_recv_borrowed(ring->raw(), h, data, size, ud);
		});
}

[[nodiscard]] wroot::Task<std::size_t> TcpStream::read_borrowed(
	std::span<std::uint8_t> dst) {
	return async_recv_borrowed(dst);
}

[[nodiscard]] wroot::Task<std::size_t> TcpStream::async_write_borrowed(
	std::span<std::uint8_t const> src) {
	return do_send(src.data(), src.size(), {});
}

[[nodiscard]] wroot::Task<std::size_t> TcpStream::async_write_copy(
	std::span<std::uint8_t const> src) {
	auto holder = std::make_shared<std::vector<std::uint8_t>>(src.begin(), src.end());
	std::uint8_t *data = holder->data();
	std::size_t const len = holder->size();
	return do_send(data, len, holder);
}

[[nodiscard]] wroot::Task<void> TcpStream::async_shutdown(
	int how) {
	auto &st = tcp_state(state_);
	if (!st.handle.valid()) {
		co_await []() -> wroot::Task<void> { throw IoError{EBADF, "tcp: stream closed"}; }();
	}
	auto task_src_3 = wroot::make_shared_task_source<void>(wroot::SubmitOptions{.enable_cancellation = false});
	auto task = std::move(task_src_3.first);
	auto shared_src = std::move(task_src_3.second);
	OsFd const h = st.handle.get();
	auto [slot, gen] = st.ring->completions().reserve([shared_src](IoResult r) mutable {
		try {
			if (r.res < 0) {
				auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{-r.res, "tcp: shutdown"}));
				return;
			}
			auto _ = shared_src->try_set_value(wroot::Success<void>{});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	});
	std::uint64_t const ud = st.ring->encode(slot, gen);
	if (!submit_shutdown(st.ring->raw(), h, how, ud)) {
		st.ring->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
	}
	co_await std::move(task);
}

[[nodiscard]] wroot::Task<void> TcpStream::async_close() {
	auto &st = tcp_state(state_);
	bool expected = false;
	if (!st.closing.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		co_return;
	}
	OsFd const h = st.handle.get();
	auto cs = std::make_shared<CloseState>();
	auto task_src_4 = wroot::make_shared_task_source<void>(wroot::SubmitOptions{.enable_cancellation = true});
	auto task = std::move(task_src_4.first);
	auto shared_src = std::move(task_src_4.second);
	[[maybe_unused]] auto _cancel = shared_src->install_cancel_hook([cs](wroot::CancelReason reason) noexcept {
		cs->cancel_reason.store(reason, std::memory_order_release);
		cs->cancel_requested.store(true, std::memory_order_relaxed);
		// No cancel SQE for close — fd released after submit; cancelling close = fd leak.
	});
	auto [slot, gen] = st.ring->completions().reserve([shared_src, cs](IoResult r) mutable {
		if (cs->cancel_requested.load(std::memory_order_relaxed)) {
			auto _ = shared_src->try_set_cancelled(cs->cancel_reason.load(std::memory_order_acquire));
		} else if (r.res < 0) {
			auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{-r.res, "tcp: close"}));
		} else {
			auto _ = shared_src->try_set_value(wroot::Success<void>{});
		}
	});
	std::uint64_t const close_ud = st.ring->encode(slot, gen);
	if (!submit_close(st.ring->raw(), h, close_ud)) {
		if (h.is_os_fd()) {
			auto _ = st.handle.release();
			::close(static_cast<int>(h.fd()));
			st.ring->completions().dispatch(slot, gen, 0, conflux::uring::CqeFlags{});
		} else {
			st.ring->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
		}
		co_await std::move(task);
		co_return;
	}
	auto _ = st.handle.release();
	co_await std::move(task);
}
[[nodiscard]] wroot::Task<void> TcpStream::async_write_all_borrowed(
	std::span<std::uint8_t const> src) {
	std::size_t sent = 0;
	while (sent < src.size()) {
		std::size_t const n = co_await async_write_borrowed({src.data() + sent, src.size() - sent});
		if (n == 0) {
			throw IoError{ECONNRESET, "tcp: connection closed"};
		}
		sent += n;
	}
}
[[nodiscard]] wroot::Task<std::size_t> TcpStream::async_write_borrowed(
	std::span<std::uint8_t const> src,
	std::chrono::milliseconds timeout) {
	if (timeout.count() == 0) {
		return async_write_borrowed(src);
	}
	auto &st = tcp_state(state_);
	if (!st.handle.valid() || st.closing.load(std::memory_order_relaxed)) {
		return wroot::make_error_task<std::size_t>(IoError{EBADF, "tcp: stream closed"});
	}
	OsFd const h = st.handle.get();
	return submit_cancellable_timeout_size_io(
		st,
		{},
		timeout,
		"tcp: send",
		"tcp: send timed out",
		[ring = st.ring,
		 h,
		 data = src.data(),
		 size = src.size()](std::uint64_t ud, std::uint64_t timeout_ud, __kernel_timespec *ts) {
			return submit_send_timeout_borrowed(ring->raw(), h, data, size, ts, ud, timeout_ud);
		});
}
[[nodiscard]] wroot::Task<void> TcpStream::async_write_all_borrowed(
	std::span<std::uint8_t const> src,
	std::chrono::milliseconds timeout) {
	std::size_t sent = 0;
	while (sent < src.size()) {
		std::size_t const n = co_await async_write_borrowed({src.data() + sent, src.size() - sent}, timeout);
		if (n == 0) {
			throw IoError{ECONNRESET, "tcp: connection closed"};
		}
		sent += n;
	}
}
[[nodiscard]] wroot::Task<void> TcpStream::async_write_all_copy(
	std::span<std::uint8_t const> src) {
	auto holder = std::make_shared<std::vector<std::uint8_t>>(src.begin(), src.end());
	std::size_t sent = 0;
	while (sent < holder->size()) {
		std::size_t const n = co_await do_send(holder->data() + sent, holder->size() - sent, holder);
		if (n == 0) {
			throw IoError{ECONNRESET, "tcp: connection closed"};
		}
		sent += n;
	}
}
[[nodiscard]] wroot::Task<std::vector<std::uint8_t>> TcpStream::async_recv_owned(
	std::size_t max_bytes) {
	std::vector<std::uint8_t> out(max_bytes);
	if (max_bytes == 0) {
		co_return out;
	}
	std::size_t const n = co_await async_recv_borrowed(std::span<std::uint8_t>{out.data(), out.size()});
	out.resize(n);
	co_return out;
}
[[nodiscard]] wroot::Task<std::size_t> TcpStream::async_write_owned(
	std::vector<std::uint8_t> data) {
	auto holder = std::make_shared<std::vector<std::uint8_t>>(std::move(data));
	std::uint8_t const *ptr = holder->data();
	std::size_t const len = holder->size();
	return do_send(ptr, len, holder);
}
[[nodiscard]] wroot::Task<std::size_t> TcpStream::async_write_owned(
	std::string data) {
	auto holder = std::make_shared<std::string>(std::move(data));
	auto const *ptr = reinterpret_cast<std::uint8_t const *>(holder->data());
	std::size_t const len = holder->size();
	return do_send(ptr, len, holder);
}
[[nodiscard]] wroot::Task<void> TcpStream::async_write_all_owned(
	std::vector<std::uint8_t> data) {
	auto holder = std::make_shared<std::vector<std::uint8_t>>(std::move(data));
	std::size_t sent = 0;
	while (sent < holder->size()) {
		std::size_t const n = co_await do_send(holder->data() + sent, holder->size() - sent, holder);
		if (n == 0) {
			throw IoError{ECONNRESET, "tcp: connection closed"};
		}
		sent += n;
	}
}
[[nodiscard]] wroot::Task<void> TcpStream::async_write_all_owned(
	std::string data) {
	auto holder = std::make_shared<std::string>(std::move(data));
	std::size_t sent = 0;
	while (sent < holder->size()) {
		auto const *ptr = reinterpret_cast<std::uint8_t const *>(holder->data() + sent);
		std::size_t const n = co_await do_send(ptr, holder->size() - sent, holder);
		if (n == 0) {
			throw IoError{ECONNRESET, "tcp: connection closed"};
		}
		sent += n;
	}
}
[[nodiscard]] wroot::Task<std::size_t> TcpStream::async_recv_borrowed(
	std::span<std::uint8_t> dst,
	std::chrono::milliseconds timeout) {
	if (timeout.count() == 0) {
		return async_recv_borrowed(dst);
	}
	auto &st = tcp_state(state_);
	if (!st.handle.valid() || st.closing.load(std::memory_order_relaxed)) {
		return wroot::make_error_task<std::size_t>(IoError{EBADF, "tcp: stream closed"});
	}
	OsFd const h = st.handle.get();
	return submit_cancellable_timeout_size_io(
		st,
		{},
		timeout,
		"tcp: recv",
		"tcp: recv timed out",
		[ring = st.ring,
		 h,
		 data = dst.data(),
		 size = dst.size()](std::uint64_t ud, std::uint64_t timeout_ud, __kernel_timespec *ts) {
			return submit_recv_timeout_borrowed(ring->raw(), h, data, size, ts, ud, timeout_ud);
		});
}
// ─── ConnectOp ───────────────────────────────────────────────────────────────

struct ConnectOp {
	enum class Stage : std::uint8_t {
		socket_pending,
		connect_pending,
		done,
	};

	SocketTaskRing *ring{};
	std::shared_ptr<wroot::TaskSource<TcpStream>> src{};
	std::shared_ptr<TcpStreamState> stream_state{};
	sockaddr_storage addr{};
	socklen_t addr_len{};
	ConnectOptions opts{};
	__kernel_timespec timeout_ts{};

	Stage stage{Stage::socket_pending};
	std::uint64_t socket_ud{};
	std::uint64_t connect_ud{};

	std::atomic_bool cancel_requested{false};
	std::atomic<wroot::CancelReason> cancel_reason{wroot::CancelReason::requested};
	std::atomic<StopCause> stop_cause{StopCause::none};
	std::atomic_bool finalized{false};
	[[nodiscard]] bool try_finalize() noexcept { return !finalized.exchange(true, std::memory_order_acq_rel); }
	void complete_exception(
		IoError e) noexcept {
		if (!try_finalize()) {
			return;
		}
		auto _ = src->try_set_exception(make_exception_ptr(std::move(e)));
	}
	void complete_cancelled() noexcept {
		if (!try_finalize()) {
			return;
		}
		auto _ = src->try_set_cancelled(cancel_reason.load(std::memory_order_acquire));
	}
	void complete_value(
		TcpStream v) noexcept {
		if (!try_finalize()) {
			return;
		}
		auto _ = src->try_set_value(wroot::Success<TcpStream>{std::move(v)});
	}
	void submit_cancel_ud_on_owner(
		SocketTaskRing &r,
		std::uint64_t target_ud,
		std::shared_ptr<ConnectOp> self) noexcept {
		auto [cs, cg] = r.completions().reserve([self](IoResult) noexcept {});
		if (!submit_cancel_by_ud(r.raw(), target_ud, r.encode(cs, cg))) {
			r.completions().dispatch(cs, cg, -EBUSY, conflux::uring::CqeFlags{});
			complete_cancelled();
			return;
		}
		auto _ = r.raw().submit();
	}
	void submit_cancel_fd_on_owner(
		SocketTaskRing &r,
		RingFd auto fd,
		std::shared_ptr<ConnectOp> self) noexcept {
		auto [cs, cg] = r.completions().reserve([self](IoResult) noexcept {});
		if (!submit_cancel_fd(r.raw(), fd, r.encode(cs, cg))) {
			r.completions().dispatch(cs, cg, -EBUSY, conflux::uring::CqeFlags{});
			complete_cancelled();
			return;
		}
		auto _ = r.raw().submit();
	}
	void cancel_on_owner(
		SocketTaskRing &r,
		std::shared_ptr<ConnectOp> self) noexcept {
		if (finalized.load(std::memory_order_acquire)) {
			return;
		}
		switch (stage) {
		case Stage::socket_pending : submit_cancel_ud_on_owner(r, socket_ud, std::move(self)); break;
		case Stage::connect_pending: submit_cancel_fd_on_owner(r, stream_state->handle.get(), std::move(self)); break;
		case Stage::done           : break;
		}
	}
	void request_cancel(
		wroot::CancelReason reason,
		std::shared_ptr<ConnectOp> self) noexcept {
		cancel_reason.store(reason, std::memory_order_release);
		stop_cause.store(StopCause::user_cancel, std::memory_order_release);
		cancel_requested.store(true, std::memory_order_release);
		if (!ring->submit_on_owner([self](SocketTaskRing &r) { self->cancel_on_owner(r, self); })) {
			complete_cancelled(); // may run on cancelling thread; relies on TaskSource setter MT-safety
		}
	}
	void on_timeout_cqe(
		IoResult r) noexcept {
		if (r.res != -ETIME) {
			return;
		}
		StopCause expected = StopCause::none;
		stop_cause.compare_exchange_strong(
			expected,
			StopCause::timeout,
			std::memory_order_acq_rel,
			std::memory_order_acquire);
	}
	void on_connect_cqe(
		IoResult r) noexcept {
		stage = Stage::done;
		if (r.res >= 0) {
			complete_value(TcpStream{std::move(stream_state)});
			return;
		}
		stream_state.reset();
		if (r.res == -ECANCELED) {
			if (cancel_requested.load(std::memory_order_acquire)
				|| stop_cause.load(std::memory_order_acquire) == StopCause::user_cancel) {
				complete_cancelled();
			} else {
				complete_exception(IoError{ETIMEDOUT, "tcp_connect: timeout"});
			}
		} else {
			complete_exception(IoError{-r.res, "tcp_connect: connect"});
		}
	}
	void on_socket_cqe(
		IoResult r,
		std::shared_ptr<ConnectOp> self) noexcept {
		if (r.res < 0) {
			if (cancel_requested.load(std::memory_order_acquire)) {
				complete_cancelled();
			} else {
				complete_exception(IoError{-r.res, "tcp_connect: socket"});
			}
			return;
		}
		auto owned = OwnedSocketHandle::from_fd(r.res);
		if (cancel_requested.load(std::memory_order_acquire)) {
			complete_cancelled();
			return;
		}
		OsFd const h = owned.get();
		if (opts.tcp_nodelay && h.is_os_fd()) {
			int const one = 1;
			if (::setsockopt(static_cast<int>(h.fd()), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
				complete_exception(IoError{errno, "tcp_connect: TCP_NODELAY"});
				return;
			}
		}
		if (opts.tcp_quickack && h.is_os_fd()) {
			int const one = 1;
			if (::setsockopt(static_cast<int>(h.fd()), IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one)) < 0) {
				complete_exception(IoError{errno, "tcp_connect: TCP_QUICKACK"});
				return;
			}
		}
		bool const use_timeout = opts.timeout > std::chrono::milliseconds{0};
		std::uint32_t const sqe_needed = use_timeout ? 2u : 1u;
		if (!ring->raw().reserve_sqe_slots(sqe_needed)) {
			complete_exception(IoError{ENOSPC, "tcp_connect: SQ full"});
			return;
		}
		stream_state = std::make_shared<TcpStreamState>(ring, std::move(owned));
		auto [cslot, cgen] = ring->completions().reserve([self](IoResult cr) noexcept { self->on_connect_cqe(cr); });
		connect_ud = ring->encode(cslot, cgen);
		std::uint32_t tslot{}, tgen{};
		if (use_timeout) {
			auto [ts, tg] = ring->completions().reserve([self](IoResult tr) noexcept { self->on_timeout_cqe(tr); });
			tslot = ts;
			tgen = tg;
		}
		stage = Stage::connect_pending;
		sockaddr const *saddr = reinterpret_cast<sockaddr const *>(&addr);
		if (!submit_connect_borrowed(ring->raw(), h, saddr, addr_len, connect_ud, use_timeout)) {
			if (use_timeout) {
				ring->completions().dispatch(tslot, tgen, -EBUSY, conflux::uring::CqeFlags{});
			}
			ring->completions().dispatch(cslot, cgen, -ENOSPC, conflux::uring::CqeFlags{});
			return;
		}
		if (use_timeout) {
			auto const sec = std::chrono::duration_cast<std::chrono::seconds>(opts.timeout);
			timeout_ts.tv_sec = sec.count();
			timeout_ts.tv_nsec = (opts.timeout - sec).count() * 1000000LL;
			if (!submit_link_timeout_borrowed(ring->raw(), &timeout_ts, ring->encode(tslot, tgen))) {
				ring->completions().dispatch(tslot, tgen, -EBUSY, conflux::uring::CqeFlags{});
				complete_exception(IoError{EBUSY, "tcp_connect: link timeout SQE unavailable"});
				return;
			}
		}
	}
};
[[nodiscard]] wroot::Task<TcpStream> async_tcp_connect(
	SocketTaskRing &ring,
	int family,
	sockaddr_storage addr,
	socklen_t len,
	ConnectOptions opts) {
	if (opts.timeout.count() < 0) {
		return wroot::make_error_task<TcpStream>(IoError{EINVAL, "tcp_connect: negative timeout"});
	}
	if (ring.opts().fd_mode == SocketFdMode::direct_required) {
		return wroot::make_error_task<TcpStream>(IoError{ENOTSUP, "tcp_connect: direct fd not yet supported"});
	}

	auto task_src_connect =
		wroot::make_shared_task_source<TcpStream>(wroot::SubmitOptions{.enable_cancellation = true});
	auto task = std::move(task_src_connect.first);
	auto src = std::move(task_src_connect.second);

	auto op = std::make_shared<ConnectOp>();
	op->ring = &ring;
	op->src = src;
	op->addr = addr;
	op->addr_len = len;
	op->opts = opts;

	auto [slot, gen] = ring.completions().reserve([op](IoResult r) noexcept { op->on_socket_cqe(r, op); });
	op->socket_ud = ring.encode(slot, gen);

	if (!submit_socket(ring.raw(), family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP, op->socket_ud)) {
		ring.completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
	}

	auto _ = src->install_cancel_hook([weak_op = std::weak_ptr<ConnectOp>{op}](wroot::CancelReason cr) noexcept {
		if (auto sop = weak_op.lock()) {
			sop->request_cancel(cr, sop);
		}
	});

	return task;
}
// ─── AcceptOp ─────────────────────────────────────────────────────────────────

struct AcceptOp {
	SocketTaskRing *ring{};
	std::shared_ptr<wroot::TaskSource<TcpStream>> src{};
	AcceptOptions opts{};
	int accept_flags{SOCK_CLOEXEC | SOCK_NONBLOCK};
	std::uint64_t accept_ud{};
	std::atomic_bool cancel_requested{false};
	std::atomic<wroot::CancelReason> cancel_reason{wroot::CancelReason::requested};
	std::atomic_bool finalized{false};
	[[nodiscard]] bool try_finalize() noexcept { return !finalized.exchange(true, std::memory_order_acq_rel); }
	void complete_exception(
		IoError e) noexcept {
		if (!try_finalize()) {
			return;
		}
		auto _ = src->try_set_exception(make_exception_ptr(std::move(e)));
	}
	void complete_cancelled() noexcept {
		if (!try_finalize()) {
			return;
		}
		auto _ = src->try_set_cancelled(cancel_reason.load(std::memory_order_acquire));
	}
	void complete_value(
		TcpStream v) noexcept {
		if (!try_finalize()) {
			return;
		}
		auto _ = src->try_set_value(wroot::Success<TcpStream>{std::move(v)});
	}
	void cancel_on_owner(
		SocketTaskRing &r,
		std::shared_ptr<AcceptOp> self) noexcept {
		if (finalized.load(std::memory_order_acquire)) {
			return;
		}
		auto [cs, cg] = r.completions().reserve([self](IoResult) noexcept {});
		if (!submit_cancel_by_ud(r.raw(), accept_ud, r.encode(cs, cg))) {
			r.completions().dispatch(cs, cg, -EBUSY, conflux::uring::CqeFlags{});
			// Flush pending SQEs to kernel to free SQ capacity, then retry.
			// ≤0: negative = ring error; 0 = nothing submitted (SQPOLL or race) → SQ still full.
			// Either way: do not retry inline or we risk unbounded recursion.
			if (r.raw().submit() <= 0) {
				return;
			}
			auto _ = r.submit_on_owner([self](SocketTaskRing &r2) noexcept { self->cancel_on_owner(r2, self); });
			return;
		}
		auto _ = r.raw().submit();
	}
	void request_cancel(
		wroot::CancelReason reason,
		std::shared_ptr<AcceptOp> self) noexcept {
		cancel_reason.store(reason, std::memory_order_release);
		cancel_requested.store(true, std::memory_order_release);
		auto _ = ring->submit_on_owner([self](SocketTaskRing &r) noexcept { self->cancel_on_owner(r, self); });
		// If enqueue fails: cancel_requested=true; on_accept_cqe drains/finalizes.
	}
	void on_accept_cqe(
		IoResult r) noexcept {
		if (r.res < 0) {
			if (cancel_requested.load(std::memory_order_acquire)) {
				complete_cancelled();
			} else {
				complete_exception(IoError{-r.res, "tcp_accept: accept"});
			}
			return;
		}
		auto owned = OwnedSocketHandle::from_fd(r.res);
		if (cancel_requested.load(std::memory_order_acquire)) {
			complete_cancelled();
			return;
		}
		OsFd const h = owned.get();
		if (opts.tcp_nodelay && h.is_os_fd()) {
			int const one = 1;
			if (::setsockopt(static_cast<int>(h.fd()), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
				complete_exception(IoError{errno, "tcp_accept: TCP_NODELAY"});
				return;
			}
		}
		if (opts.tcp_quickack && h.is_os_fd()) {
			int const one = 1;
			if (::setsockopt(static_cast<int>(h.fd()), IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one)) < 0) {
				complete_exception(IoError{errno, "tcp_accept: TCP_QUICKACK"});
				return;
			}
		}
		complete_value(TcpStream{std::make_shared<TcpStreamState>(ring, std::move(owned))});
	}
};
// ─── tcp_accept ───────────────────────────────────────────────────────────────
// Precondition: listener and ring must outlive the returned Task until
// completion or cancellation has fully drained.

[[nodiscard]] wroot::Task<TcpStream> async_tcp_accept(
	TcpListener &listener,
	SocketTaskRing &ring,
	AcceptOptions opts) {
	if (ring.opts().fd_mode == SocketFdMode::direct_required) {
		return wroot::make_error_task<TcpStream>(IoError{ENOTSUP, "tcp_accept: direct fd"});
	}

	auto task_src_accept = wroot::make_shared_task_source<TcpStream>(wroot::SubmitOptions{.enable_cancellation = true});
	auto task = std::move(task_src_accept.first);
	auto src = std::move(task_src_accept.second);

	auto op = std::make_shared<AcceptOp>();
	op->ring = &ring;
	op->src = src;
	op->opts = opts;
	op->accept_flags = listener.accept_flags();

	auto [slot, gen] = ring.completions().reserve([op](IoResult r) noexcept { op->on_accept_cqe(r); });
	op->accept_ud = ring.encode(slot, gen);

	if (!submit_accept_borrowed(ring.raw(), listener.handle(), nullptr, nullptr, op->accept_ud, op->accept_flags)) {
		ring.completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
	}

	auto _ = src->install_cancel_hook([weak_op = std::weak_ptr<AcceptOp>{op}](wroot::CancelReason cr) noexcept {
		if (auto sop = weak_op.lock()) {
			sop->request_cancel(cr, sop);
		}
	});

	return task;
}
// ─── MultishotAcceptOp ────────────────────────────────────────────────────────

struct MultishotAcceptOp {
	SocketTaskRing *ring{};
	std::shared_ptr<wroot::TaskSource<void>> src{};
	AcceptOptions opts{};
	int accept_flags{SOCK_CLOEXEC | SOCK_NONBLOCK};
	OsFd listen_fd{};
	std::uint64_t accept_ud{};
	std::function<wroot::Task<void>(TcpStream)> handler{};
	std::atomic_bool cancel_requested{false};
	std::atomic<wroot::CancelReason> cancel_reason{wroot::CancelReason::requested};
	std::atomic_bool finalized{false};
	[[nodiscard]] bool try_finalize() noexcept { return !finalized.exchange(true, std::memory_order_acq_rel); }
	void complete_exception(
		IoError e) noexcept {
		if (!try_finalize()) {
			return;
		}
		auto _ = src->try_set_exception(make_exception_ptr(std::move(e)));
	}
	void complete_cancelled() noexcept {
		if (!try_finalize()) {
			return;
		}
		auto _ = src->try_set_cancelled(cancel_reason.load(std::memory_order_acquire));
	}
	void cancel_on_owner(
		SocketTaskRing &r,
		std::shared_ptr<MultishotAcceptOp> self) noexcept {
		if (finalized.load(std::memory_order_acquire)) {
			return;
		}
		auto [cs, cg] = r.completions().reserve([self](IoResult) noexcept {});
		if (!submit_cancel_fd(r.raw(), listen_fd, r.encode(cs, cg))) {
			r.completions().dispatch(cs, cg, -EBUSY, conflux::uring::CqeFlags{});
			// Flush pending SQEs to kernel to free SQ capacity, then retry.
			// ≤0: negative = ring error; 0 = nothing submitted (SQPOLL or race) → SQ still full.
			// Either way: do not retry inline or we risk unbounded recursion.
			if (r.raw().submit() <= 0) {
				return;
			}
			auto _ = r.submit_on_owner([self](SocketTaskRing &r2) noexcept { self->cancel_on_owner(r2, self); });
			return;
		}
		auto _ = r.raw().submit();
	}
	void request_cancel(
		wroot::CancelReason reason,
		std::shared_ptr<MultishotAcceptOp> self) noexcept {
		cancel_reason.store(reason, std::memory_order_release);
		cancel_requested.store(true, std::memory_order_release);
		auto _ = ring->submit_on_owner([self](SocketTaskRing &r) noexcept { self->cancel_on_owner(r, self); });
		// If enqueue fails: cancel_requested=true; on_accept_cqe drains/finalizes.
	}
	void on_accept_cqe(
		IoResult r,
		std::shared_ptr<MultishotAcceptOp> self) noexcept {
		bool const more = r.flags.any(conflux::uring::cqe_flags::more);
		if (r.res < 0) {
			if (cancel_requested.load(std::memory_order_acquire) || r.res == -ECANCELED) {
				complete_cancelled();
			} else {
				complete_exception(IoError{-r.res, "tcp_accept_multishot: accept"});
			}
			return;
		}
		auto owned = OwnedSocketHandle::from_fd(r.res);
		if (cancel_requested.load(std::memory_order_acquire)) {
			if (!more) {
				complete_cancelled();
			}
			return;
		}
		if (opts.tcp_nodelay && owned.get().is_os_fd()) {
			int const one = 1;
			::setsockopt(static_cast<int>(owned.get().fd()), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		}
		if (opts.tcp_quickack && owned.get().is_os_fd()) {
			int const one = 1;
			::setsockopt(static_cast<int>(owned.get().fd()), IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
		}
		try {
			handler(TcpStream{std::make_shared<TcpStreamState>(ring, std::move(owned))}).detach();
		} catch (std::bad_alloc const &) {
			complete_exception(IoError{ENOMEM, "tcp_accept_multishot: handler allocation"});
			cancel_requested.store(true, std::memory_order_release);
			if (more) {
				try {
					auto [cs, cg] = ring->completions().reserve([](IoResult) noexcept {});
					if (!submit_cancel_fd(ring->raw(), listen_fd, ring->encode(cs, cg))) {
						ring->completions().dispatch(cs, cg, -EBUSY, conflux::uring::CqeFlags{});
					} else {
						auto _ = ring->raw().submit();
					}
				} catch (...) {} // NOLINT(bugprone-empty-catch): best-effort cancel after handler allocation failure;
								 // primary error already stored.
			}
			return;
		} catch (...) {
			complete_exception(IoError{EIO, "tcp_accept_multishot: handler threw"});
			cancel_requested.store(true, std::memory_order_release);
			if (more) {
				try {
					auto [cs, cg] = ring->completions().reserve([](IoResult) noexcept {});
					if (!submit_cancel_fd(ring->raw(), listen_fd, ring->encode(cs, cg))) {
						ring->completions().dispatch(cs, cg, -EBUSY, conflux::uring::CqeFlags{});
					} else {
						auto _ = ring->raw().submit();
					}
				} catch (...) {} // NOLINT(bugprone-empty-catch): best-effort cancel after handler exception; primary
								 // error already stored.
			}
			return;
		}
		if (!more) {
			if (cancel_requested.load(std::memory_order_acquire)) {
				complete_cancelled();
				return;
			}
			auto [slot, gen] =
				ring->completions().reserve_multishot([self](IoResult r2) noexcept { self->on_accept_cqe(r2, self); });
			accept_ud = ring->encode(slot, gen);
			if (!submit_accept_multishot_borrowed(
					ring->raw(),
					listen_fd,
					nullptr,
					nullptr,
					accept_ud,
					accept_flags,
					false)) {
				ring->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
			}
		}
	}
};
// ─── tcp_accept_multishot ─────────────────────────────────────────────────────
// Preconditions:
//   - listener must outlive the returned Task until co_await resolves.
//   - ring.opts().submit_on_ring_owner must be set if Task::cancel() is called
//     from any thread other than the ring owner (the normal case).
//   - While bound to listener, no other io_uring op may target the same listener fd;
//     cancel_on_owner uses IORING_ASYNC_CANCEL_FD which cancels all ops on the fd.

[[nodiscard]] wroot::Task<void> async_tcp_accept_multishot(
	TcpListener &listener,
	SocketTaskRing &ring,
	AcceptOptions opts,
	std::function<wroot::Task<void>(TcpStream)> handler) {
	if (ring.opts().fd_mode == SocketFdMode::direct_required) {
		return wroot::make_error_task<void>(IoError{ENOTSUP, "tcp_accept_multishot: direct fd"});
	}
	if (!handler) {
		return wroot::make_error_task<void>(IoError{EINVAL, "tcp_accept_multishot: empty handler"});
	}

	auto task_src_multishot = wroot::make_shared_task_source<void>(wroot::SubmitOptions{.enable_cancellation = true});
	auto task = std::move(task_src_multishot.first);
	auto src = std::move(task_src_multishot.second);

	auto op = std::make_shared<MultishotAcceptOp>();
	op->ring = &ring;
	op->src = src;
	op->opts = opts;
	op->accept_flags = listener.accept_flags();
	op->listen_fd = listener.handle();
	op->handler = std::move(handler);

	auto [slot, gen] = ring.completions().reserve_multishot([op](IoResult r) noexcept { op->on_accept_cqe(r, op); });
	op->accept_ud = ring.encode(slot, gen);

	if (!submit_accept_multishot_borrowed(
			ring.raw(),
			listener.handle(),
			nullptr,
			nullptr,
			op->accept_ud,
			op->accept_flags,
			false)) {
		ring.completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
	}

	auto _ =
		src->install_cancel_hook([weak_op = std::weak_ptr<MultishotAcceptOp>{op}](wroot::CancelReason cr) noexcept {
			if (auto sop = weak_op.lock()) {
				sop->request_cancel(cr, sop);
			}
		});

	return task;
}
// ─── UdpRecvResult ───────────────────────────────────────────────────────────

struct MsgHolder {
	msghdr msg{};
	iovec iov{};
	sockaddr_storage from{};
	std::vector<std::uint8_t> payload{};
	sockaddr_storage to{};
};

UdpSocket::UdpSocket() noexcept = default;
UdpSocket::UdpSocket(
	SocketTaskRing &ring,
	OwnedSocketHandle fh) noexcept
	: ring_{&ring}
	, handle_{std::move(fh)} {}
UdpSocket::~UdpSocket() = default;
UdpSocket::UdpSocket(UdpSocket &&) noexcept = default;
UdpSocket &UdpSocket::operator =(UdpSocket &&) noexcept = default;

[[nodiscard]] bool UdpSocket::valid() const noexcept {
	return ring_ != nullptr && handle_.valid();
}

[[nodiscard]] int UdpSocket::raw_fd() const noexcept {
	return handle_.raw_fd();
}

[[nodiscard]] UdpSocket UdpSocket::ephemeral(
	SocketTaskRing &ring,
	int family) {
	int const fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
	if (fd < 0) {
		throw IoError{errno, "udp: socket"};
	}
	if (family == AF_INET) {
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_ANY);
		if (::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0) {
			int const e = errno;
			::close(fd);
			throw IoError{e, "udp: bind"};
		}
	} else if (family == AF_INET6) {
		sockaddr_in6 sa{};
		sa.sin6_family = AF_INET6;
		sa.sin6_addr = in6addr_any;
		if (::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0) {
			int const e = errno;
			::close(fd);
			throw IoError{e, "udp: bind"};
		}
	} else {
		::close(fd);
		throw IoError{EAFNOSUPPORT, "udp: unsupported family"};
	}
	return UdpSocket{ring, OwnedSocketHandle::from_fd(fd)};
}
[[nodiscard]] wroot::Task<std::size_t> UdpSocket::async_send_to_borrowed(
	std::span<std::uint8_t const> data,
	sockaddr_storage addr,
	socklen_t addr_len) {
	auto task_src_7 = wroot::make_shared_task_source<std::size_t>(wroot::SubmitOptions{.enable_cancellation = false});
	auto task = std::move(task_src_7.first);
	auto shared_src = std::move(task_src_7.second);
	OsFd const h = handle_.get();
	struct SendHolder {
		msghdr msg{};
		iovec iov{};
		sockaddr_storage to{};
	};
	auto holder = std::make_shared<SendHolder>();
	holder->to = addr;
	holder->iov.iov_base = const_cast<void *>(static_cast<void const *>(data.data()));
	holder->iov.iov_len = data.size();
	holder->msg.msg_name = &holder->to;
	holder->msg.msg_namelen = addr_len;
	holder->msg.msg_iov = &holder->iov;
	holder->msg.msg_iovlen = 1;
	auto [slot, gen] = ring_->completions().reserve([shared_src, holder](IoResult r) mutable {
		try {
			if (r.res < 0) {
				auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{-r.res, "udp: sendto"}));
				return;
			}
			auto _ = shared_src->try_set_value(wroot::Success<std::size_t>{static_cast<std::size_t>(r.res)});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	});
	std::uint64_t const ud = ring_->encode(slot, gen);
	if (!submit_sendmsg_borrowed(ring_->raw(), h, &holder->msg, ud)) {
		ring_->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
	}
	co_return co_await std::move(task);
}
[[nodiscard]] wroot::Task<std::size_t> UdpSocket::async_send_to_copy(
	std::span<std::uint8_t const> data,
	sockaddr_storage addr,
	socklen_t addr_len) {
	auto task_src_8 = wroot::make_shared_task_source<std::size_t>(wroot::SubmitOptions{.enable_cancellation = false});
	auto task = std::move(task_src_8.first);
	auto shared_src = std::move(task_src_8.second);
	OsFd const h = handle_.get();
	struct SendCopyHolder {
		std::vector<std::uint8_t> payload;
		msghdr msg{};
		iovec iov{};
		sockaddr_storage to{};
	};
	auto holder = std::make_shared<SendCopyHolder>();
	holder->payload.assign(data.begin(), data.end());
	holder->to = addr;
	holder->iov.iov_base = holder->payload.empty() ? nullptr : holder->payload.data();
	holder->iov.iov_len = holder->payload.size();
	holder->msg.msg_name = &holder->to;
	holder->msg.msg_namelen = addr_len;
	holder->msg.msg_iov = &holder->iov;
	holder->msg.msg_iovlen = 1;
	auto [slot, gen] = ring_->completions().reserve([shared_src, holder](IoResult r) mutable {
		try {
			if (r.res < 0) {
				auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{-r.res, "udp: sendto"}));
				return;
			}
			auto _ = shared_src->try_set_value(wroot::Success<std::size_t>{static_cast<std::size_t>(r.res)});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	});
	std::uint64_t const ud = ring_->encode(slot, gen);
	if (!submit_sendmsg_borrowed(ring_->raw(), h, &holder->msg, ud)) {
		ring_->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
	}
	co_return co_await std::move(task);
}
[[nodiscard]] wroot::Task<UdpRecvResult> UdpSocket::async_recv_from(
	std::span<std::uint8_t> buf) {
	auto task_src_9 = wroot::make_shared_task_source<UdpRecvResult>(wroot::SubmitOptions{.enable_cancellation = false});
	auto task = std::move(task_src_9.first);
	auto shared_src = std::move(task_src_9.second);
	OsFd const h = handle_.get();
	auto holder = std::make_shared<MsgHolder>();
	holder->iov.iov_base = buf.data();
	holder->iov.iov_len = buf.size();
	holder->msg.msg_name = &holder->from;
	holder->msg.msg_namelen = sizeof(holder->from);
	holder->msg.msg_iov = &holder->iov;
	holder->msg.msg_iovlen = 1;
	auto [slot, gen] = ring_->completions().reserve([shared_src, holder](IoResult r) mutable {
		try {
			if (r.res < 0) {
				auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{-r.res, "udp: recvfrom"}));
				return;
			}
			UdpRecvResult result;
			result.bytes = static_cast<std::size_t>(r.res);
			result.from = holder->from;
			result.from_len = holder->msg.msg_namelen;
			auto _ = shared_src->try_set_value(wroot::Success<UdpRecvResult>{std::move(result)});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	});
	std::uint64_t const ud = ring_->encode(slot, gen);
	if (!submit_recvmsg_borrowed(ring_->raw(), h, &holder->msg, ud)) {
		ring_->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
	}
	co_return co_await std::move(task);
}
[[nodiscard]] wroot::Task<UdpRecvResult> UdpSocket::async_recv_from(
	std::span<std::uint8_t> buf,
	std::chrono::milliseconds timeout) {
	if (timeout.count() < 0) {
		return wroot::make_error_task<UdpRecvResult>(IoError{EINVAL, "udp: negative timeout"});
	}
	auto task_src_10 = wroot::make_shared_task_source<UdpRecvResult>(wroot::SubmitOptions{.enable_cancellation = true});
	auto task = std::move(task_src_10.first);
	auto shared_src = std::move(task_src_10.second);
	OsFd const h = handle_.get();
	auto holder = std::make_shared<MsgHolder>();
	holder->iov.iov_base = buf.data();
	holder->iov.iov_len = buf.size();
	holder->msg.msg_name = &holder->from;
	holder->msg.msg_namelen = sizeof(holder->from);
	holder->msg.msg_iov = &holder->iov;
	holder->msg.msg_iovlen = 1;
	auto ts = make_kernel_timespec(timeout);
	auto state = std::make_shared<RecvTimeoutState>();
	auto [slot, gen] = ring_->completions().reserve([shared_src, holder, state](IoResult r) mutable {
		try {
			if (r.res == -ECANCELED) {
				auto cause = state->stop_cause.load(std::memory_order_acquire);
				if (cause == StopCause::user_cancel) {
					auto _ = shared_src->try_set_cancelled(state->reason());
				} else {
					auto _ =
						shared_src->try_set_exception(make_exception_ptr(IoError{ETIMEDOUT, "udp: recv timed out"}));
				}
				return;
			}
			if (r.res < 0) {
				auto _ = shared_src->try_set_exception(make_exception_ptr(IoError{-r.res, "udp: recvfrom"}));
				return;
			}
			UdpRecvResult result;
			result.bytes = static_cast<std::size_t>(r.res);
			result.from = holder->from;
			result.from_len = holder->msg.msg_namelen;
			auto _ = shared_src->try_set_value(wroot::Success<UdpRecvResult>{std::move(result)});
		} catch (...) { auto _ = shared_src->try_set_exception(std::current_exception()); }
	});
	std::uint64_t const recv_ud = ring_->encode(slot, gen);
	auto [tslot, tgen] = ring_->completions().reserve([ts, state](IoResult r) mutable {
		if (r.res == -ETIME) {
			state->mark_stop(StopCause::timeout);
		}
		auto _ = ts;
	});
	std::uint64_t const timeout_ud = ring_->encode(tslot, tgen);
	if (!submit_recvmsg_timeout_borrowed(ring_->raw(), h, &holder->msg, ts.get(), recv_ud, timeout_ud)) {
		ring_->completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
		ring_->completions().dispatch(tslot, tgen, -EBUSY, conflux::uring::CqeFlags{});
		return task;
	}
	auto ring_ptr = ring_;
	auto weak_src = std::weak_ptr<wroot::TaskSource<UdpRecvResult>>{shared_src};
	auto _ = shared_src->install_cancel_hook([ring_ptr, recv_ud, weak_src, state](wroot::CancelReason reason) noexcept {
		state->mark_cancel(reason);
		submit_cancel_for_ud(ring_ptr, recv_ud, weak_src, state->reason());
	});
	return task;
}
[[nodiscard]] wroot::Task<void> async_sleep_for(
	SocketTaskRing &ring,
	std::chrono::milliseconds dur) {
	if (dur.count() <= 0) {
		auto [task, src] = wroot::make_task_source<void>(wroot::SubmitOptions{.enable_cancellation = false});
		(void)src.try_set_value(wroot::Success<void>{});
		return std::move(task);
	}
	auto task_src_11 = wroot::make_shared_task_source<void>(wroot::SubmitOptions{.enable_cancellation = true});
	auto task = std::move(task_src_11.first);
	auto shared_src = std::move(task_src_11.second);
	auto cancel_reason = std::make_shared<std::atomic<wroot::CancelReason>>(wroot::CancelReason::requested);
	auto ts = make_kernel_timespec(dur);
	auto [slot, gen] = ring.completions().reserve([shared_src, ts, cancel_reason](IoResult r) mutable {
		auto _ = ts;
		if (r.res == -ECANCELED) {
			auto _ = shared_src->try_set_cancelled(cancel_reason->load(std::memory_order_acquire));
		} else {
			auto _ = shared_src->try_set_value(wroot::Success<void>{});
		}
	});
	std::uint64_t const ud = ring.encode(slot, gen);
	if (!submit_timeout_borrowed(ring.raw(), ts.get(), ud)) {
		ring.completions().dispatch(slot, gen, -ENOSPC, conflux::uring::CqeFlags{});
		return task;
	}
	auto ring_ptr = &ring;
	auto weak_src = std::weak_ptr<wroot::TaskSource<void>>{shared_src};
	auto _ =
		shared_src->install_cancel_hook([ring_ptr, ud, weak_src, cancel_reason](wroot::CancelReason reason) noexcept {
			cancel_reason->store(reason, std::memory_order_release);
			complete_cancel_fallback(weak_src, reason);
			submit_cancel_for_ud(ring_ptr, ud, weak_src, reason);
		});
	return task;
}

[[nodiscard]] wroot::Task<void> timeout_after(
	SocketTaskRing &ring,
	std::chrono::milliseconds dur) {
	return async_sleep_for(ring, dur);
}

[[nodiscard]] wroot::Task<void> timeout_at(
	SocketTaskRing &ring,
	std::chrono::steady_clock::time_point deadline) {
	auto const now = std::chrono::steady_clock::now();
	if (deadline <= now) {
		co_return;
	}
	auto const millis = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
	co_await async_sleep_for(ring, millis);
}

} // namespace conflux::socket_io
