module;

#include <cassert>

export module conflux.work.carrier.scope;

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier;
export namespace conflux::work::carrier {

// Scope provides parent-triggered best-effort cancellation for a group of
// concurrent tasks. Track controls before joining; cancel() fires
// request_cancel() on all tracked controls concurrently with any blocking join.
//
// Thread-safe: track(), cancel(), and admit() may be called from any std::thread.
// The std::mutex is not held during root::blocking_join() to avoid blocking cancel() callers.
class Scope {
	mutable std::mutex mu_;
	std::vector<root::TaskControl> task_ctrls_;
	std::vector<root::PostedControl> posted_ctrls_;
	std::vector<root::OperationControl> op_ctrls_;
	bool cancelled_ = false;
	root::CancelReason cancel_reason_ = root::CancelReason::requested;

public:
	Scope() noexcept = default;
	~Scope() = default;

	Scope(Scope &&) = delete;
	Scope(Scope const &) = delete;
	Scope &operator =(Scope &&) = delete;
	Scope &operator =(Scope const &) = delete;
	// Add a control handle for cancel propagation.
	// Throws std::bad_alloc if tracking storage cannot be extended.
	// If scope is already cancelled, fires request_cancel() immediately without
	// storing the handle.
	void track(
		root::TaskControl ctrl) {
		{
			std::unique_lock lock{mu_};
			if (!cancelled_) {
#ifdef CONFLUX_WORK_CHECKED_BUILD
				assert(
					(task_ctrls_.size() + posted_ctrls_.size() + op_ctrls_.size()) < 32
					&& "Scope::track exceeded n=32; partition across multiple Scope instances");
#endif
				task_ctrls_.push_back(std::move(ctrl));
				return;
			}
			lock.unlock();
		}
		auto _ = ctrl.request_cancel();
	}
	void track(
		root::PostedControl ctrl) {
		{
			std::unique_lock lock{mu_};
			if (!cancelled_) {
#ifdef CONFLUX_WORK_CHECKED_BUILD
				assert(
					(task_ctrls_.size() + posted_ctrls_.size() + op_ctrls_.size()) < 32
					&& "Scope::track exceeded n=32; partition across multiple Scope instances");
#endif
				posted_ctrls_.push_back(std::move(ctrl));
				return;
			}
			lock.unlock();
		}
		auto _ = ctrl.request_cancel();
	}
	void track(
		root::OperationControl ctrl) {
		{
			std::unique_lock lock{mu_};
			if (!cancelled_) {
#ifdef CONFLUX_WORK_CHECKED_BUILD
				assert(
					(task_ctrls_.size() + posted_ctrls_.size() + op_ctrls_.size()) < 32
					&& "Scope::track exceeded n=32; partition across multiple Scope instances");
#endif
				op_ctrls_.push_back(std::move(ctrl));
				return;
			}
			lock.unlock();
		}
		auto _ = ctrl.request_cancel();
	}
	void cancel(
		root::CancelReason reason) noexcept {
		std::vector<root::TaskControl> task;
		std::vector<root::PostedControl> posted;
		std::vector<root::OperationControl> op;
		{
			std::lock_guard const lock{mu_};
			if (cancelled_) {
				return;
			}
			cancelled_ = true;
			cancel_reason_ = reason;
			task.swap(task_ctrls_);
			posted.swap(posted_ctrls_);
			op.swap(op_ctrls_);
		}
		for (auto &c: task) {
			auto _ = c.request_cancel();
		}
		for (auto &c: posted) {
			auto _ = c.request_cancel();
		}
		for (auto &c: op) {
			auto _ = c.request_cancel();
		}
	}
	[[nodiscard]] bool is_cancelled() const noexcept {
		std::lock_guard const lock{mu_};
		return cancelled_;
	}
	[[nodiscard]] root::CancelReason cancel_reason() const noexcept {
		std::lock_guard const lock{mu_};
		return cancel_reason_;
	}
	// Track the join handle's control then join: cancel() fired concurrently
	// will signal the task to cancel, causing root::join to return Cancelled.
	template<root::work_value T>
	[[nodiscard]] Chain<T> admit(
		root::TaskJoinHandle<T> &&jh) {
		track(jh.control());
		return Chain<T>{root::blocking_join(std::move(jh)), CarrierKind::task};
	}
	template<root::work_value T, root::progress_capability Owner>
	[[nodiscard]] Chain<T> admit(
		Owner &owner,
		root::PostedJoinHandle<T> &&jh) {
		track(jh.control());
		return Chain<T>{root::blocking_join(owner, std::move(jh)), CarrierKind::posted, root::capability_id(owner)};
	}
	template<root::work_value T, root::progress_capability Owner>
	[[nodiscard]] Chain<T> admit_unbound(
		Owner &owner,
		root::PostedJoinHandle<T> &&jh) {
		track(jh.control());
		return Chain<T>{root::blocking_join(owner, std::move(jh)), CarrierKind::posted};
	}
	template<root::work_value T, root::progress_capability Driver>
	[[nodiscard]] Chain<T> admit(
		Driver &driver,
		root::OperationJoinHandle<T> &&jh) {
		track(jh.control());
		return Chain<T>{
			root::blocking_join(driver, std::move(jh)),
			CarrierKind::operation,
			root::capability_id(driver)};
	}
	template<root::work_value T, root::progress_capability Driver>
	[[nodiscard]] Chain<T> admit_unbound(
		Driver &driver,
		root::OperationJoinHandle<T> &&jh) {
		track(jh.control());
		return Chain<T>{root::blocking_join(driver, std::move(jh)), CarrierKind::operation};
	}
};

} // namespace conflux::work::carrier
