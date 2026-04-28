module;

#include <cassert>

export module conflux.work.carrier.scope;

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.model_a;

export namespace conflux::work::carrier {

// Scope provides parent-triggered best-effort cancellation for a group of
// concurrent tasks. Track controls before joining; cancel() fires
// request_cancel() on all tracked controls concurrently with any blocking join.
//
// Thread-safe: track(), cancel(), and admit() may be called from any thread.
// The mutex is not held during root::join() to avoid blocking cancel() callers.
class Scope {
	mutable std::mutex mu_;
	V<root::TaskControl> task_ctrls_;
	V<root::PostedControl> posted_ctrls_;
	V<root::OperationControl> op_ctrls_;
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
				assert(
					(task_ctrls_.size() + posted_ctrls_.size() + op_ctrls_.size()) < 32
					&& "Scope::track exceeded n=32; partition across multiple Scope instances "
					   "or file a follow-up for concurrent registry");
				task_ctrls_.push_back(std::move(ctrl));
				return;
			}
			lock.unlock();
		}
		(void)ctrl.request_cancel();
	}

	void track(
		root::PostedControl ctrl) {
		{
			std::unique_lock lock{mu_};
			if (!cancelled_) {
				assert(
					(task_ctrls_.size() + posted_ctrls_.size() + op_ctrls_.size()) < 32
					&& "Scope::track exceeded n=32; partition across multiple Scope instances "
					   "or file a follow-up for concurrent registry");
				posted_ctrls_.push_back(std::move(ctrl));
				return;
			}
			lock.unlock();
		}
		(void)ctrl.request_cancel();
	}

	void track(
		root::OperationControl ctrl) {
		{
			std::unique_lock lock{mu_};
			if (!cancelled_) {
				assert(
					(task_ctrls_.size() + posted_ctrls_.size() + op_ctrls_.size()) < 32
					&& "Scope::track exceeded n=32; partition across multiple Scope instances "
					   "or file a follow-up for concurrent registry");
				op_ctrls_.push_back(std::move(ctrl));
				return;
			}
			lock.unlock();
		}
		(void)ctrl.request_cancel();
	}

	void cancel(
		root::CancelReason reason) noexcept {
		V<root::TaskControl> task;
		V<root::PostedControl> posted;
		V<root::OperationControl> op;
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
			(void)c.request_cancel();
		}
		for (auto &c: posted) {
			(void)c.request_cancel();
		}
		for (auto &c: op) {
			(void)c.request_cancel();
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
	[[nodiscard]] model_a::Chain<T> admit(
		root::TaskJoinHandle<T> &&jh) {
		track(jh.control());
		return model_a::Chain<T>{root::join(std::move(jh)), model_a::CarrierKind::task};
	}

	template<root::work_value T, root::progress_capability Owner>
	[[nodiscard]] model_a::Chain<T> admit(
		Owner &owner,
		root::PostedJoinHandle<T> &&jh) {
		track(jh.control());
		return model_a::Chain<T>{
			root::join(owner, std::move(jh)),
			model_a::CarrierKind::posted,
			root::capability_id(owner)};
	}

	template<root::work_value T, root::progress_capability Owner>
	[[nodiscard]] model_a::Chain<T> admit_unbound(
		Owner &owner,
		root::PostedJoinHandle<T> &&jh) {
		track(jh.control());
		return model_a::Chain<T>{root::join(owner, std::move(jh)), model_a::CarrierKind::posted};
	}

	template<root::work_value T, root::progress_capability Driver>
	[[nodiscard]] model_a::Chain<T> admit(
		Driver &driver,
		root::OperationJoinHandle<T> &&jh) {
		track(jh.control());
		return model_a::Chain<T>{
			root::join(driver, std::move(jh)),
			model_a::CarrierKind::operation,
			root::capability_id(driver)};
	}

	template<root::work_value T, root::progress_capability Driver>
	[[nodiscard]] model_a::Chain<T> admit_unbound(
		Driver &driver,
		root::OperationJoinHandle<T> &&jh) {
		track(jh.control());
		return model_a::Chain<T>{root::join(driver, std::move(jh)), model_a::CarrierKind::operation};
	}
};

} // namespace conflux::work::carrier
