module;

#include <cassert>
#include <liburing.h>
#include <memory>
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
		u64 cqe_tag) {
		tfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		if (tfd_ < 0) {
			throw SE{errno, system_category(), "timerfd_create"};
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
		chrono::steady_clock::time_point deadline;
		u64 expected_gen;
		SP<u64> gen;
		Fn<void()> fn;
		bool operator >(
			Entry const &o) const noexcept {
			return deadline > o.deadline;
		}
	};
	io_uring *ring_ = nullptr;
	u64 cqe_tag_{};
	thread::id owner_{};
	int tfd_ = -1;
	u64 read_buf_{};
	std::priority_queue<Entry, V<Entry>, std::greater<Entry>> heap_;
	u64 tombstone_count_ = 0;
	u64 cancel_count_ = 0;
	[[nodiscard]] SP<u64> insert_(
		chrono::steady_clock::time_point deadline,
		Fn<void()> fn) {
		check_thread_();
		auto gen = make_shared<u64>(0);
		bool const was_empty = heap_.empty();
		bool const is_new_min = !was_empty && deadline < heap_.top().deadline;
		heap_.push(Entry{deadline, 0, gen, move(fn)});
		if (was_empty || is_new_min) {
			rearm_();
		}
		maybe_compact_();
		return gen;
	}
	void cancel_(
		SP<u64> const &gen) noexcept {
		check_thread_();
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
		auto const now = chrono::steady_clock::now();
		auto delta = top.deadline - now;
		if (delta < chrono::nanoseconds{0}) {
			delta = chrono::nanoseconds{0};
		}
		auto const sec = chrono::duration_cast<chrono::seconds>(delta);
		auto const nsec = chrono::duration_cast<chrono::nanoseconds>(delta - sec);
		itimerspec its{};
		its.it_value.tv_sec = sec.count();
		its.it_value.tv_nsec = nsec.count();
		if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) {
			its.it_value.tv_nsec = 1;
		}
		::timerfd_settime(tfd_, 0, &its, nullptr);
	}
	void fire_expired_() noexcept {
		auto const now = chrono::steady_clock::now();
		while (!heap_.empty()) {
			auto &top = const_cast<Entry &>(heap_.top());
			if (top.deadline > now) {
				break;
			}
			if (*top.gen == top.expected_gen) {
				auto fn = move(top.fn);
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
			auto e = move(const_cast<Entry &>(heap_.top()));
			heap_.pop();
			if (*e.gen == e.expected_gen) {
				survivors.push_back(move(e));
			}
		}
		for (auto &s: survivors) {
			heap_.push(move(s));
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
template<class Clock = chrono::steady_clock>
class LaneTimerScope {
public:
	LaneTimerScope(
		TimerService &svc,
		typename Clock::time_point deadline,
		Fn<void()> cancel_fn)
		: svc_{&svc} {
		auto const steady_deadline = [&] {
			if constexpr (same_as<Clock, chrono::steady_clock>) {
				return deadline;
			} else {
				auto const delta = deadline - Clock::now();
				return chrono::steady_clock::now() + delta;
			}
		}();
		gen_ = svc.insert_(steady_deadline, move(cancel_fn));
	}
	~LaneTimerScope() noexcept { cancel_(); }
	LaneTimerScope(
		LaneTimerScope &&o) noexcept
		: svc_{o.svc_}
		, gen_{exchange(o.gen_, {})} {}
	LaneTimerScope &operator =(
		LaneTimerScope &&o) noexcept {
		if (this != &o) {
			cancel_();
			svc_ = o.svc_;
			gen_ = exchange(o.gen_, {});
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
	SP<u64> gen_;
};

} // namespace conflux::work::carrier
