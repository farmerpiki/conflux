module;

// Sanitizer detection → disable pool when running under ASAN/TSAN/MSAN
#if CONFLUX_WORK_CORO_FRAME_POOL
	#if defined(__has_feature)
		#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
			#define CONFLUX_WORK_CFP_ACTIVE 0
		#else
			#define CONFLUX_WORK_CFP_ACTIVE 1
		#endif
	#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
		#define CONFLUX_WORK_CFP_ACTIVE 0
	#else
		#define CONFLUX_WORK_CFP_ACTIVE 1
	#endif
#else
	#define CONFLUX_WORK_CFP_ACTIVE 0
#endif
#if CONFLUX_WORK_CFP_ACTIVE
	#include <sys/mman.h>
#endif

export module conflux.work.carrier.coro;

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;

// ---------------------------------------------------------------------------
// Per-std::thread monotonic bump arena for EagerChain coroutine frames.
// Layout: [kAlign-std::byte header (marker in std::byte 0) | frame (sz bytes)]
// Alignment: all returned pointers are __STDCPP_DEFAULT_NEW_ALIGNMENT__-aligned.
// Reclaim is LIFO: nested EagerChains (never suspend) produce strict stack order.
// ---------------------------------------------------------------------------
#if CONFLUX_WORK_CFP_ACTIVE
namespace conflux::work::carrier::pool {

struct FrameArena {
	static constexpr std::size_t kCap = 8u * 1024u * 1024u;
	static constexpr std::size_t kDefaultAlign = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

	struct Header {
		FrameArena *arena;
		void *raw;
		std::size_t previous_top;
		std::size_t next_top;
		std::size_t allocation_size;
		std::size_t alignment;
		bool pooled;
	};

	char *base_ = nullptr;
	std::size_t top_ = 0;
	std::size_t pool_alloc_count_ = 0;
	std::size_t fallback_count_ = 0;
	std::size_t largest_frame_ = 0;
	std::mutex mtx_{};

	FrameArena() noexcept {
		void *p = mmap(nullptr, kCap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p != MAP_FAILED) {
			base_ = static_cast<char *>(p);
		}
	}
	FrameArena(FrameArena const &) = delete;
	FrameArena &operator =(FrameArena const &) = delete;
	static constexpr std::size_t align_up(
		std::size_t n,
		std::size_t a) noexcept {
		return (n + a - 1u) & ~(a - 1u);
	}
	[[nodiscard]] void *alloc(
		std::size_t sz,
		std::size_t requested_align) {
		std::size_t const alignment = std::max({requested_align, alignof(Header), kDefaultAlign});
		if (sz > std::numeric_limits<std::size_t>::max() - sizeof(Header) - (alignment - 1u)) {
			throw std::bad_alloc{};
		}
		auto const payload_from = [](char *raw, std::size_t align) {
			auto start = reinterpret_cast<std::uintptr_t>(raw + sizeof(Header));
			return reinterpret_cast<char *>(align_up(start, align));
		};
		{
			std::scoped_lock const lk{mtx_};
			if (base_) {
				char *payload = payload_from(base_ + top_, alignment);
				auto const next_top = align_up(static_cast<std::size_t>((payload + sz) - base_), kDefaultAlign);
				if (next_top <= kCap) {
					auto *hdr = reinterpret_cast<Header *>(payload - sizeof(Header));
					*hdr = Header{
						.arena = this,
						.raw = nullptr,
						.previous_top = top_,
						.next_top = next_top,
						.allocation_size = sz,
						.alignment = alignment,
						.pooled = true};
					top_ = next_top;
					++pool_alloc_count_;
					if (sz > largest_frame_) {
						largest_frame_ = sz;
					}
					return payload;
				}
			}
		}
		return fallback_alloc(sz, alignment, payload_from);
	}
	template<class PayloadFrom>
	[[nodiscard]] void *fallback_alloc(
		std::size_t sz,
		std::size_t alignment,
		PayloadFrom payload_from) {
		std::size_t const bytes = sizeof(Header) + sz + alignment - 1u;
		void *raw = ::operator new(bytes, std::align_val_t{alignment});
		char *payload = payload_from(static_cast<char *>(raw), alignment);
		auto *hdr = reinterpret_cast<Header *>(payload - sizeof(Header));
		*hdr = Header{
			.arena = nullptr,
			.raw = raw,
			.previous_top = 0,
			.next_top = 0,
			.allocation_size = sz,
			.alignment = alignment,
			.pooled = false};
		{
			std::scoped_lock const lk{mtx_};
			++fallback_count_;
			if (sz > largest_frame_) {
				largest_frame_ = sz;
			}
		}
		return payload;
	}
	void dealloc_pooled(
		Header *hdr) noexcept {
		std::scoped_lock const lk{mtx_};
		if (top_ == hdr->next_top) {
			top_ = hdr->previous_top;
		}
	}
};
[[nodiscard]] inline FrameArena &frame_arena() noexcept {
	// EagerChain is movable across threads. Keep arena metadata and mmaps alive
	// for process lifetime so a later cross-thread destroy can read the frame
	// header and return storage to the owning arena without touching unmapped
	// thread-local state.
	thread_local FrameArena *arena = new FrameArena;
	return *arena;
}
inline void dealloc_frame(
	void *ptr) noexcept {
	if (ptr == nullptr) {
		return;
	}
	auto *hdr = reinterpret_cast<FrameArena::Header *>(static_cast<char *>(ptr) - sizeof(FrameArena::Header));
	if (hdr->pooled) {
		hdr->arena->dealloc_pooled(hdr);
		return;
	}
	::operator delete(hdr->raw, std::align_val_t{hdr->alignment});
}

} // namespace conflux::work::carrier::pool
#endif
export namespace conflux::work::carrier {

template<root::work_value T>
class EagerChain;

} // namespace conflux::work::carrier
namespace conflux::work::carrier {

template<root::work_value T>
struct EagerChainPromise {
	std::optional<root::Outcome<T>> slot_{};

	EagerChain<T> get_return_object() noexcept;
	std::suspend_never initial_suspend() noexcept { return {}; }
	std::suspend_always final_suspend() noexcept { return {}; }
	void unhandled_exception() { slot_ = root::Outcome<T>{root::Failure{std::current_exception()}}; }
	void return_value(
		T v) {
		slot_ = root::Outcome<T>{root::Success<T>{std::move(v)}};
	}
	template<root::work_value U>
	ChainAwaiter<U> await_transform(
		Chain<U> &&c) noexcept {
		return std::move(c).operator co_await();
	}
	template<root::work_value U>
	ChainAwaiter<U> await_transform(EagerChain<U> &&e) noexcept;

	template<class Awaitable>
	void await_transform(Awaitable &&) = delete;

#if CONFLUX_WORK_CFP_ACTIVE
	[[nodiscard]] static void *operator new(
		std::size_t sz) {
		return pool::frame_arena().alloc(sz, pool::FrameArena::kDefaultAlign);
	}
	[[nodiscard]] static void *operator new(
		std::size_t sz,
		std::align_val_t align) {
		return pool::frame_arena().alloc(sz, static_cast<std::size_t>(align));
	}
	static void operator delete(
		void *ptr,
		std::size_t) noexcept {
		pool::dealloc_frame(ptr);
	}
	static void operator delete(
		void *ptr,
		std::size_t,
		std::align_val_t) noexcept {
		pool::dealloc_frame(ptr);
	}
#endif
};
template<>
struct EagerChainPromise<void> {
	std::optional<root::Outcome<void>> slot_{};

	EagerChain<void> get_return_object() noexcept;
	std::suspend_never initial_suspend() noexcept { return {}; }
	std::suspend_always final_suspend() noexcept { return {}; }
	void unhandled_exception() { slot_ = root::Outcome<void>{root::Failure{std::current_exception()}}; }
	void return_void() { slot_ = root::Outcome<void>{root::Success<void>{}}; }
	template<root::work_value U>
	ChainAwaiter<U> await_transform(
		Chain<U> &&c) noexcept {
		return std::move(c).operator co_await();
	}
	template<root::work_value U>
	ChainAwaiter<U> await_transform(EagerChain<U> &&e) noexcept;

	template<class Awaitable>
	void await_transform(Awaitable &&) = delete;

#if CONFLUX_WORK_CFP_ACTIVE
	[[nodiscard]] static void *operator new(
		std::size_t sz) {
		return pool::frame_arena().alloc(sz, pool::FrameArena::kDefaultAlign);
	}
	[[nodiscard]] static void *operator new(
		std::size_t sz,
		std::align_val_t align) {
		return pool::frame_arena().alloc(sz, static_cast<std::size_t>(align));
	}
	static void operator delete(
		void *ptr,
		std::size_t) noexcept {
		pool::dealloc_frame(ptr);
	}
	static void operator delete(
		void *ptr,
		std::size_t,
		std::align_val_t) noexcept {
		pool::dealloc_frame(ptr);
	}
#endif
};

} // namespace conflux::work::carrier
export namespace conflux::work::carrier {

template<root::work_value T>
class EagerChain {
	using promise_t = ::conflux::work::carrier::EagerChainPromise<T>;
	std::coroutine_handle<promise_t> handle_;

public:
	using promise_type = promise_t;
	explicit EagerChain(
		std::coroutine_handle<promise_t> h) noexcept
		: handle_{h} {}
	~EagerChain() noexcept {
		if (handle_) {
			handle_.destroy();
		}
	}
	EagerChain(
		EagerChain &&o) noexcept
		: handle_{std::exchange(o.handle_, {})} {}
	EagerChain &operator =(
		EagerChain &&o) noexcept {
		if (this != &o) {
			if (handle_) {
				handle_.destroy();
			}
			handle_ = std::exchange(o.handle_, {});
		}
		return *this;
	}
	EagerChain(EagerChain const &) = delete;
	EagerChain &operator =(EagerChain const &) = delete;
	[[nodiscard]] ChainAwaiter<T> operator co_await() && noexcept {
		return std::move(*this).chain().operator co_await();
	}
	[[nodiscard]] Chain<T> chain() && {
		auto &p = handle_.promise();
		if (!p.slot_) {
			auto ex = std::make_exception_ptr(
				root::WorkError{"EagerChain suspended: body awaited an asynchronous awaitable"});
			handle_.destroy();
			handle_ = {};
			return Chain<T>{root::Outcome<T>{root::Failure{ex}}, CarrierKind::task};
		}
		auto out = std::move(*p.slot_);
		handle_.destroy();
		handle_ = {};
		return Chain<T>{std::move(out), CarrierKind::task};
	}
};

} // namespace conflux::work::carrier
namespace conflux::work::carrier {

template<root::work_value T>
EagerChain<T> EagerChainPromise<T>::get_return_object() noexcept {
	return EagerChain<T>{std::coroutine_handle<EagerChainPromise<T>>::from_promise(*this)};
}
EagerChain<void> EagerChainPromise<void>::get_return_object() noexcept {
	return EagerChain<void>{std::coroutine_handle<EagerChainPromise<void>>::from_promise(*this)};
}
template<root::work_value T>
template<root::work_value U>
ChainAwaiter<U> EagerChainPromise<T>::await_transform(
	EagerChain<U> &&e) noexcept {
	return std::move(e).chain().operator co_await();
}
template<root::work_value U>
ChainAwaiter<U> EagerChainPromise<void>::await_transform(
	EagerChain<U> &&e) noexcept {
	return std::move(e).chain().operator co_await();
}

} // namespace conflux::work::carrier
export namespace conflux::work::carrier {

template<root::work_value T>
class TaskHandleAwaiter {
	root::TaskJoinHandle<T> handle_;
	root::BasicControl<root::ControlCategory::task> control_;
	enum class AwaiterError : std::uint8_t {
		none,
		already_installed,
		empty,
	};
	AwaiterError error_ = AwaiterError::none;
	bool handle_consumed_ = false;
	bool callback_installed_ = false;

public:
	explicit TaskHandleAwaiter(
		root::TaskJoinHandle<T> &&h) noexcept
		: handle_{std::move(h)}
		, control_{handle_.control()} {}
	~TaskHandleAwaiter() noexcept {
		if (handle_consumed_ || !bool(handle_)) {
			return;
		}
		if (callback_installed_) {
			auto status = control_.clear_on_ready();
			if (status == root::ClearOnReadyStatus::in_flight) {
#ifdef CONFLUX_WORK_CHECKED_BUILD
				root::emit_carrier_diagnostic_fmt(
					"TaskHandleAwaiter dtor raced commit's in-flight callback "
					"— UB possible if coroutine frame is also being destroyed "
					"(awaiter=%p)",
					static_cast<void *>(this));
#endif
			}
		}
		auto _ = root::try_abandon_to(std::move(handle_), root::drop_on_abandon{});
#ifdef CONFLUX_WORK_CHECKED_BUILD
		root::emit_carrier_diagnostic("TaskHandleAwaiter destroyed unconsumed — defensive abandon");
#endif
	}
	[[nodiscard]] bool await_ready() const noexcept { return control_.ready(); }
	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = control_.try_set_on_ready([h]() mutable noexcept { h.resume(); });
		switch (result.status) {
		case root::ReadyRegistration::installed        : callback_installed_ = true; return true;
		case root::ReadyRegistration::already_ready    : return false;
		case root::ReadyRegistration::already_installed: error_ = AwaiterError::already_installed; return false;
		case root::ReadyRegistration::empty            : error_ = AwaiterError::empty; return false;
		}
		return false;
	}
	T await_resume() {
		if (error_ == AwaiterError::already_installed) {
			auto _ = root::try_abandon_to(std::move(handle_), root::drop_on_abandon{});
			handle_consumed_ = true;
			throw root::JoinError{root::JoinError::reason::ready_callback_already_installed};
		}
		if (error_ == AwaiterError::empty) {
			handle_consumed_ = true;
			throw root::JoinError{root::JoinError::reason::consumed_handle};
		}
		auto out = root::join_ready(std::move(handle_));
		handle_consumed_ = true;
		if (out.is_success()) {
			if constexpr (std::same_as<T, void>) {
				return;
			} else {
				return std::move(out).success().value;
			}
		}
		if (out.is_failure()) {
			std::rethrow_exception(std::move(out).failure().error);
		}
		throw root::CancelledError{out.cancelled().reason};
	}
};
template<root::work_value T>
class TaskHandleChainAwaiter {
	root::TaskJoinHandle<T> handle_;
	root::BasicControl<root::ControlCategory::task> control_;
	enum class AwaiterError : std::uint8_t {
		none,
		already_installed,
		empty,
	};
	AwaiterError error_ = AwaiterError::none;
	bool handle_consumed_ = false;
	bool callback_installed_ = false;

public:
	explicit TaskHandleChainAwaiter(
		root::TaskJoinHandle<T> &&h) noexcept
		: handle_{std::move(h)}
		, control_{handle_.control()} {}
	~TaskHandleChainAwaiter() noexcept {
		if (handle_consumed_ || !bool(handle_)) {
			return;
		}
		if (callback_installed_) {
			auto status = control_.clear_on_ready();
			if (status == root::ClearOnReadyStatus::in_flight) {
#ifdef CONFLUX_WORK_CHECKED_BUILD
				root::emit_carrier_diagnostic_fmt(
					"TaskHandleChainAwaiter dtor raced commit's in-flight callback "
					"(awaiter=%p)",
					static_cast<void *>(this));
#endif
			}
		}
		auto _ = root::try_abandon_to(std::move(handle_), root::drop_on_abandon{});
#ifdef CONFLUX_WORK_CHECKED_BUILD
		root::emit_carrier_diagnostic("TaskHandleChainAwaiter destroyed unconsumed — defensive abandon");
#endif
	}
	[[nodiscard]] bool await_ready() const noexcept { return control_.ready(); }
	[[nodiscard]] bool await_suspend(
		std::coroutine_handle<> h) noexcept {
		auto result = control_.try_set_on_ready([h]() mutable noexcept { h.resume(); });
		switch (result.status) {
		case root::ReadyRegistration::installed        : callback_installed_ = true; return true;
		case root::ReadyRegistration::already_ready    : return false;
		case root::ReadyRegistration::already_installed: error_ = AwaiterError::already_installed; return false;
		case root::ReadyRegistration::empty            : error_ = AwaiterError::empty; return false;
		}
		return false;
	}
	Chain<T> await_resume() {
		if (error_ == AwaiterError::already_installed) {
			auto _ = root::try_abandon_to(std::move(handle_), root::drop_on_abandon{});
			handle_consumed_ = true;
			auto ex =
				std::make_exception_ptr(root::JoinError{root::JoinError::reason::ready_callback_already_installed});
			return Chain<T>{root::Outcome<T>{root::Failure{ex}}, CarrierKind::task};
		}
		if (error_ == AwaiterError::empty) {
			handle_consumed_ = true;
			auto ex = std::make_exception_ptr(root::JoinError{root::JoinError::reason::consumed_handle});
			return Chain<T>{root::Outcome<T>{root::Failure{ex}}, CarrierKind::task};
		}
		auto out = root::join_ready(std::move(handle_));
		handle_consumed_ = true;
		return Chain<T>{std::move(out), CarrierKind::task};
	}
};
template<root::work_value T>
[[nodiscard]] TaskHandleAwaiter<T> operator co_await(
	root::TaskJoinHandle<T> &&jh) noexcept {
	return TaskHandleAwaiter<T>{std::move(jh)};
}
template<root::work_value T>
[[nodiscard]] TaskHandleChainAwaiter<T> await_chain(
	root::TaskJoinHandle<T> &&jh) noexcept {
	return TaskHandleChainAwaiter<T>{std::move(jh)};
}
template<root::work_value T>
[[deprecated(
	"co_await on PostedJoinHandle requires owner-affine resumption "
	"(not yet implemented). FROM A NON-COROUTINE CONTEXT use "
	"Scope::admit to obtain a Chain<T> (admit BLOCKS — do NOT call "
	"from inside a coroutine), then co_await the Chain.")]] auto
operator co_await(root::PostedJoinHandle<T> &&) = delete;

template<root::work_value T>
[[deprecated(
	"co_await on OperationJoinHandle requires driver-affine "
	"resumption (not yet implemented). FROM A NON-COROUTINE CONTEXT "
	"use Scope::admit to obtain a Chain<T> (admit BLOCKS — do NOT "
	"call from inside a coroutine), then co_await the Chain.")]] auto
operator co_await(root::OperationJoinHandle<T> &&) = delete;

} // namespace conflux::work::carrier
