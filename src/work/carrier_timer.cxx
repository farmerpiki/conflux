module;

#include <cassert>
#include <liburing.h>
#include <sys/timerfd.h>
#include <unistd.h>

export module conflux.work.carrier.timer;

import std;
import conflux.types;
import conflux.work.root;

export namespace conflux::work::carrier {

template<class Clock>
class LaneTimerScope;

class TimerService {
public:
	explicit TimerService(
		io_uring *ring,
		std::uint64_t cqe_tag) {
		tfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		if (tfd_ < 0) {
			throw std::system_error{errno, std::system_category(), "timerfd_create"};
		}
		ring_ = ring;
		cqe_tag_ = cqe_tag;
		owner_ = std::this_thread::get_id();
		submit_read_();
	}

	~TimerService() noexcept {
		if (tfd_ >= 0) {
			::close(tfd_);
		}
	}

	TimerService(TimerService const &) = delete;
	TimerService &operator =(TimerService const &) = delete;
	TimerService(TimerService &&) = delete;
	TimerService &operator =(TimerService &&) = delete;

	void on_cqe(
		io_uring_cqe const *cqe) noexcept {
		check_thread_();
		if (cqe->res < 0) {
			if (cqe->res == -ECANCELED) {
				return;
			}
			root::emit_carrier_diagnostic("TimerService: timerfd read CQE error");
			return;
		}
		fire_expired_();
		if (!heap_.empty()) {
			rearm_();
		}
		submit_read_();
	}

	[[nodiscard]] bool on_owner_thread() const noexcept { return std::this_thread::get_id() == owner_; }

	[[nodiscard]] int timer_fd() const noexcept { return tfd_; }

private:
	template<class Clock>
	friend class LaneTimerScope;

	struct Entry {
		std::chrono::steady_clock::time_point deadline;
		std::uint64_t expected_gen;
		SP<std::uint64_t> gen;
		Fn<void()> fn;

		bool operator >(
			Entry const &o) const noexcept {
			return deadline > o.deadline;
		}
	};

	io_uring *ring_ = nullptr;
	std::uint64_t cqe_tag_{};
	std::thread::id owner_{};
	int tfd_ = -1;
	std::uint64_t read_buf_{};
	std::priority_queue<Entry, V<Entry>, std::greater<Entry>> heap_;
	std::uint64_t tombstone_count_ = 0;
	std::uint64_t cancel_count_ = 0;

	[[nodiscard]] SP<std::uint64_t> insert_(
		std::chrono::steady_clock::time_point deadline,
		Fn<void()> fn) {
		check_thread_();
		auto gen = std::make_shared<std::uint64_t>(0);
		bool const was_empty = heap_.empty();
		bool const is_new_min = !was_empty && deadline < heap_.top().deadline;
		heap_.push(Entry{deadline, 0, gen, std::move(fn)});
		if (was_empty || is_new_min) {
			rearm_();
		}
		maybe_compact_();
		return gen;
	}

	void cancel_(
		SP<std::uint64_t> const &gen) noexcept {
		if (!gen) {
			return;
		}
		++(*gen);
		++tombstone_count_;
		++cancel_count_;
		maybe_compact_();
	}

	void rearm_() noexcept {
		if (heap_.empty()) {
			return;
		}
		auto const &top = heap_.top();
		auto const now = std::chrono::steady_clock::now();
		auto delta = top.deadline - now;
		if (delta < std::chrono::nanoseconds{0}) {
			delta = std::chrono::nanoseconds{0};
		}
		auto const sec = std::chrono::duration_cast<std::chrono::seconds>(delta);
		auto const nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(delta - sec);
		itimerspec its{};
		its.it_value.tv_sec = sec.count();
		its.it_value.tv_nsec = nsec.count();
		if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) {
			its.it_value.tv_nsec = 1;
		}
		::timerfd_settime(tfd_, 0, &its, nullptr);
	}

	void fire_expired_() noexcept {
		auto const now = std::chrono::steady_clock::now();
		while (!heap_.empty()) {
			auto &top = const_cast<Entry &>(heap_.top());
			if (top.deadline > now) {
				break;
			}
			if (*top.gen == top.expected_gen) {
				auto fn = std::move(top.fn);
				heap_.pop();
				if (fn) {
					try {
						fn();
					} catch (...) {
						root::emit_carrier_diagnostic("TimerService: timer callback threw — exception swallowed");
					}
				}
			} else {
				heap_.pop();
			}
		}
	}

	void maybe_compact_() noexcept {
		bool const by_ratio = !heap_.empty() && tombstone_count_ > heap_.size() / 2;
		bool const by_count = cancel_count_ >= 1024;
		if (!by_ratio && !by_count) {
			return;
		}
		try {
			compact_();
		} catch (...) { root::emit_carrier_diagnostic("TimerService: compaction failed — heap not compacted"); }
	}

	void compact_() {
		V<Entry> survivors;
		while (!heap_.empty()) {
			auto e = std::move(const_cast<Entry &>(heap_.top()));
			heap_.pop();
			if (*e.gen == e.expected_gen) {
				survivors.push_back(std::move(e));
			}
		}
		for (auto &s: survivors) {
			heap_.push(std::move(s));
		}
		tombstone_count_ = 0;
		cancel_count_ = 0;
	}

	void submit_read_() noexcept {
		io_uring_sqe *sqe = io_uring_get_sqe(ring_);
		if (sqe == nullptr) {
			root::emit_carrier_diagnostic("TimerService: io_uring_get_sqe failed — timerfd read not resubmitted");
			return;
		}
		io_uring_prep_read(sqe, tfd_, &read_buf_, sizeof(read_buf_), 0);
		io_uring_sqe_set_data64(sqe, cqe_tag_);
		io_uring_submit(ring_);
	}

	void check_thread_() noexcept {
#ifdef CONFLUX_WORK_CHECKED_BUILD
		assert(std::this_thread::get_id() == owner_);
#else
		if (std::this_thread::get_id() != owner_) {
			root::emit_carrier_diagnostic("TimerService: cross-thread access detected");
		}
#endif
	}
};

template<class Clock = std::chrono::steady_clock>
class LaneTimerScope {
public:
	LaneTimerScope(
		TimerService &svc,
		typename Clock::time_point deadline,
		Fn<void()> cancel_fn)
		: svc_{&svc} {
		auto const steady_deadline = [&] {
			if constexpr (std::same_as<Clock, std::chrono::steady_clock>) {
				return deadline;
			} else {
				auto const delta = deadline - Clock::now();
				return std::chrono::steady_clock::now() + delta;
			}
		}();
		gen_ = svc.insert_(steady_deadline, std::move(cancel_fn));
	}

	~LaneTimerScope() noexcept { cancel_(); }

	LaneTimerScope(
		LaneTimerScope &&o) noexcept
		: svc_{o.svc_}
		, gen_{std::exchange(o.gen_, {})} {}

	LaneTimerScope &operator =(
		LaneTimerScope &&o) noexcept {
		if (this != &o) {
			cancel_();
			svc_ = o.svc_;
			gen_ = std::exchange(o.gen_, {});
		}
		return *this;
	}

	LaneTimerScope(LaneTimerScope const &) = delete;
	LaneTimerScope &operator =(LaneTimerScope const &) = delete;

private:
	void cancel_() noexcept {
		if (gen_ != nullptr && svc_ != nullptr) {
			svc_->cancel_(gen_);
			gen_.reset();
		}
	}

	TimerService *svc_ = nullptr;
	SP<std::uint64_t> gen_;
};

} // namespace conflux::work::carrier
