module;

export module conflux.work.carrier.deadline;

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.scope;

export namespace conflux::work::carrier {

class DeadlineScope : public Scope {
	std::jthread timer_;

public:
	explicit DeadlineScope(
		std::chrono::steady_clock::time_point deadline) {
		timer_ = std::jthread{[this, deadline](std::stop_token const &st) {
			std::mutex mu;
			std::condition_variable_any cv;
			std::stop_callback const cb{st, [&cv] { cv.notify_one(); }};
			std::unique_lock lk{mu};
			cv.wait_until(lk, st, deadline, [] { return false; });
			if (!st.stop_requested()) {
				cancel(root::CancelReason::deadline);
			}
		}};
	}

	template<class Rep, class Period>
	explicit DeadlineScope(
		std::chrono::duration<Rep, Period> timeout)
		: DeadlineScope(
			  std::chrono::steady_clock::now()
			  + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout)) {}

	~DeadlineScope() = default;

	DeadlineScope(DeadlineScope &&) = delete;
	DeadlineScope(DeadlineScope const &) = delete;
	DeadlineScope &operator =(DeadlineScope &&) = delete;
	DeadlineScope &operator =(DeadlineScope const &) = delete;
};

} // namespace conflux::work::carrier
