export module conflux.net.cancel;

import std;

import conflux.types;
import conflux.work;

namespace wroot = conflux::work::root;

export struct ActiveTaskCancelRelay {
	void *state_{};

	ActiveTaskCancelRelay();
	~ActiveTaskCancelRelay();
	ActiveTaskCancelRelay(ActiveTaskCancelRelay const &) = delete;
	ActiveTaskCancelRelay &operator =(ActiveTaskCancelRelay const &) = delete;
	ActiveTaskCancelRelay(ActiveTaskCancelRelay &&other) noexcept;
	ActiveTaskCancelRelay &operator =(ActiveTaskCancelRelay &&other) noexcept;

	void set_active(wroot::TaskControl c);
	void clear_active() noexcept;
	void cancel() noexcept;
	[[nodiscard]] bool is_cancelled() const noexcept;
	void throw_if_cancelled() const;

	[[nodiscard]] wroot::Task<std::size_t> await_child(wroot::Task<std::size_t> child);
	[[nodiscard]] wroot::Task<void> await_child(wroot::Task<void> child);
};
