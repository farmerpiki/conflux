module;

export module conflux.work.carrier;

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.flags;
namespace conflux::work::carrier::detail {

template<class O>
struct outcome_value_impl;
template<root::work_value T>
struct outcome_value_impl<root::Outcome<T>> {
	using type = T;
};
template<class O>
using outcome_value_t = typename outcome_value_impl<std::remove_cvref_t<O>>::type;
template<root::work_value T, class Fn>
	requires(!same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
auto map_outcome(
	root::Outcome<T> out,
	Fn &&fn) -> root::Outcome<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
	if (out.is_failure()) {
		return root::Outcome<U>{move(out).failure()};
	}
	if (out.is_cancelled()) {
		return root::Outcome<U>{move(out).cancelled()};
	}
	try {
		if constexpr (same_as<U, void>) {
			invoke(forward<Fn>(fn), move(out).success().value);
			return root::Outcome<U>{root::Success<U>{}};
		} else {
			auto result = invoke(forward<Fn>(fn), move(out).success().value);
			return root::Outcome<U>{root::Success<U>{move(result)}};
		}
	} catch (...) { return root::Outcome<U>{root::Failure{current_exception()}}; }
}

} // namespace conflux::work::carrier::detail
export namespace conflux::work::carrier {

template<root::work_value T>
struct ChainAwaiter;

enum class CarrierKind : u8 {
	task,
	posted,
	operation,
};
class HopCapabilityError : public root::JoinError {
public:
	HopCapabilityError()
		: JoinError{root::JoinError::reason::hop_capability_mismatch} {}
};
class AggregateError : public root::WorkError {
	V<EP> causes_;

public:
	explicit AggregateError(
		V<EP> causes)
		: WorkError{"carrier: multiple failures"}
		, causes_{move(causes)} {}
	[[nodiscard]] V<EP> causes_owned() const { return causes_; }
	[[nodiscard("span lifetime bound to *this — moves invalidate")]] span<EP const> causes_view() const noexcept {
		return causes_;
	}
};
template<root::work_value T>
class Chain {
	root::Outcome<T> outcome_;
	CarrierKind kind_ = CarrierKind::task;
	root::CapabilityId bound_cap_{};

public:
	Chain() = delete;
	Chain(
		root::Outcome<T> outcome,
		CarrierKind kind) noexcept
		: outcome_{move(outcome)}
		, kind_{kind} {}
	Chain(
		root::Outcome<T> outcome,
		CarrierKind kind,
		root::CapabilityId cap) noexcept
		: outcome_{move(outcome)}
		, kind_{kind}
		, bound_cap_{cap} {}
	Chain(Chain &&) noexcept = default;
	Chain &operator =(Chain &&) noexcept = default;
	Chain(Chain const &) = delete;
	Chain &operator =(Chain const &) = delete;
	[[nodiscard]] CarrierKind kind() const noexcept { return kind_; }
	[[nodiscard]] root::CapabilityId bound_capability() const noexcept { return bound_cap_; }
	[[nodiscard]] root::Outcome<T> release_outcome() && noexcept { return move(outcome_); }
	[[nodiscard]] ChainAwaiter<T> operator co_await() && noexcept;
	// --- E1.z combinators ---

	// success → f(value) → Chain<R>; failure/cancel pass through
	template<class Fn>
		requires(same_as<T, void>
				 && std::invocable<Fn &>
				 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &>>>)
			 || (!same_as<T, void>
				 && std::invocable<Fn &, T>
				 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>)
	[[nodiscard]] auto then(
		Fn &&fn) && {
		if constexpr (same_as<T, void>) {
			using R = std::remove_cvref_t<std::invoke_result_t<Fn &>>;
			if (outcome_.is_failure()) {
				return Chain<R>{root::Outcome<R>{move(outcome_).failure()}, kind_, bound_cap_};
			}
			if (outcome_.is_cancelled()) {
				return Chain<R>{root::Outcome<R>{move(outcome_).cancelled()}, kind_, bound_cap_};
			}
			if constexpr (std::is_nothrow_invocable_v<Fn &>) {
				if constexpr (same_as<R, void>) {
					invoke(forward<Fn>(fn));
					return Chain<R>{root::Outcome<R>{root::Success<R>{}}, kind_, bound_cap_};
				} else {
					return Chain<R>{root::Outcome<R>{root::Success<R>{invoke(forward<Fn>(fn))}}, kind_, bound_cap_};
				}
			} else {
				try {
					if constexpr (same_as<R, void>) {
						invoke(forward<Fn>(fn));
						return Chain<R>{root::Outcome<R>{root::Success<R>{}}, kind_, bound_cap_};
					} else {
						return Chain<R>{root::Outcome<R>{root::Success<R>{invoke(forward<Fn>(fn))}}, kind_, bound_cap_};
					}
				} catch (...) {
					return Chain<R>{root::Outcome<R>{root::Failure{current_exception()}}, kind_, bound_cap_};
				}
			}
		} else {
			using R = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
			if (outcome_.is_failure()) {
				return Chain<R>{root::Outcome<R>{move(outcome_).failure()}, kind_, bound_cap_};
			}
			if (outcome_.is_cancelled()) {
				return Chain<R>{root::Outcome<R>{move(outcome_).cancelled()}, kind_, bound_cap_};
			}
			if constexpr (std::is_nothrow_invocable_v<Fn &, T>) {
				if constexpr (same_as<R, void>) {
					invoke(forward<Fn>(fn), move(outcome_).success().value);
					return Chain<R>{root::Outcome<R>{root::Success<R>{}}, kind_, bound_cap_};
				} else {
					return Chain<R>{
						root::Outcome<R>{root::Success<R>{invoke(forward<Fn>(fn), move(outcome_).success().value)}},
						kind_,
						bound_cap_};
				}
			} else {
				try {
					if constexpr (same_as<R, void>) {
						invoke(forward<Fn>(fn), move(outcome_).success().value);
						return Chain<R>{root::Outcome<R>{root::Success<R>{}}, kind_, bound_cap_};
					} else {
						return Chain<R>{
							root::Outcome<R>{root::Success<R>{invoke(forward<Fn>(fn), move(outcome_).success().value)}},
							kind_,
							bound_cap_};
					}
				} catch (...) {
					return Chain<R>{root::Outcome<R>{root::Failure{current_exception()}}, kind_, bound_cap_};
				}
			}
		}
	}
	// failure → f(exception_ptr) → T or Chain<T>; success/cancel pass through
	template<class Fn>
		requires std::invocable<Fn &, EP>
			  && (same_as<std::remove_cvref_t<std::invoke_result_t<Fn &, EP>>, T>
				  || same_as<std::remove_cvref_t<std::invoke_result_t<Fn &, EP>>, Chain<T>>)
	[[nodiscard]] Chain<T> catch_error(
		Fn &&fn) && {
		using R = std::remove_cvref_t<std::invoke_result_t<Fn &, std::exception_ptr>>;
		if (!outcome_.is_failure()) {
			return Chain<T>{move(outcome_), kind_, bound_cap_};
		}
		auto ep = outcome_.failure().error;
		try {
			if constexpr (same_as<R, Chain<T>>) {
				return invoke(forward<Fn>(fn), ep);
			} else if constexpr (same_as<T, void>) {
				invoke(forward<Fn>(fn), ep);
				return Chain<T>{root::Outcome<T>{root::Success<T>{}}, kind_, bound_cap_};
			} else {
				return Chain<T>{root::Outcome<T>{root::Success<T>{invoke(forward<Fn>(fn), ep)}}, kind_, bound_cap_};
			}
		} catch (...) { return Chain<T>{root::Outcome<T>{root::Failure{current_exception()}}, kind_, bound_cap_}; }
	}
	// cancelled → f() side-effect; still cancelled. success/failure pass through.
	template<class Fn>
		requires std::invocable<Fn &>
	[[nodiscard]] Chain<T> on_cancel(
		Fn &&fn) && noexcept {
		if (!outcome_.is_cancelled()) {
			return Chain<T>{move(outcome_), kind_, bound_cap_};
		}
		try {
			invoke(forward<Fn>(fn));
		} catch (...) {} // ignore cancellation observer failures
		return Chain<T>{move(outcome_), kind_, bound_cap_};
	}
	// cancelled → f() → T or Chain<T> (becomes success); success/failure pass through
	template<class Fn>
		requires std::invocable<Fn &>
			  && (same_as<std::remove_cvref_t<std::invoke_result_t<Fn &>>, T>
				  || same_as<std::remove_cvref_t<std::invoke_result_t<Fn &>>, Chain<T>>)
	[[nodiscard]] Chain<T> recover_cancel(
		Fn &&fn) && {
		using R = std::remove_cvref_t<std::invoke_result_t<Fn &>>;
		if (!outcome_.is_cancelled()) {
			return Chain<T>{move(outcome_), kind_, bound_cap_};
		}
		try {
			if constexpr (same_as<R, Chain<T>>) {
				return invoke(forward<Fn>(fn));
			} else if constexpr (same_as<T, void>) {
				invoke(forward<Fn>(fn));
				return Chain<T>{root::Outcome<T>{root::Success<T>{}}, kind_, bound_cap_};
			} else {
				return Chain<T>{root::Outcome<T>{root::Success<T>{invoke(forward<Fn>(fn))}}, kind_, bound_cap_};
			}
		} catch (...) { return Chain<T>{root::Outcome<T>{root::Failure{current_exception()}}, kind_, bound_cap_}; }
	}
	// failure or cancel → f(Outcome<T>) → T; success passes through
	template<class Fn>
		requires std::invocable<Fn &, root::Outcome<T>>
			  && same_as<std::remove_cvref_t<std::invoke_result_t<Fn &, root::Outcome<T>>>, T>
	[[nodiscard]] Chain<T> recover(
		Fn &&fn) && {
		if (outcome_.is_success()) {
			return Chain<T>{move(outcome_), kind_, bound_cap_};
		}
		try {
			if constexpr (same_as<T, void>) {
				invoke(forward<Fn>(fn), move(outcome_));
				return Chain<T>{root::Outcome<T>{root::Success<T>{}}, kind_, bound_cap_};
			} else {
				return Chain<T>{
					root::Outcome<T>{root::Success<T>{invoke(forward<Fn>(fn), move(outcome_))}},
					kind_,
					bound_cap_};
			}
		} catch (...) { return Chain<T>{root::Outcome<T>{root::Failure{current_exception()}}, kind_, bound_cap_}; }
	}
	// any outcome → f(Outcome<T>) → Outcome<R> → Chain<R> (outcome-preserving transform)
	template<class Fn>
		requires std::invocable<Fn &, root::Outcome<T>>
	[[nodiscard]] auto transform_outcome(
		Fn &&fn) && {
		using OutR = std::remove_cvref_t<std::invoke_result_t<Fn &, root::Outcome<T>>>;
		using R = detail::outcome_value_t<OutR>;
		try {
			return Chain<R>{invoke(forward<Fn>(fn), move(outcome_)), kind_, bound_cap_};
		} catch (...) { return Chain<R>{root::Outcome<R>{root::Failure{current_exception()}}, kind_, bound_cap_}; }
	}
	// bind chain execution to capability (outcome-preserving)
	template<root::progress_capability Cap>
	[[nodiscard]] Chain<T> schedule_on(
		Cap &target) && noexcept {
		return Chain<T>{move(outcome_), kind_, root::capability_id(target)};
	}
	// success hop to target + transform; non-success pass through
	template<root::progress_capability Cap, class Fn>
		requires(same_as<T, void>
				 && std::invocable<Fn &>
				 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &>>>)
			 || (!same_as<T, void>
				 && std::invocable<Fn &, T>
				 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>)
	[[nodiscard]] auto then_on(
		Cap &target,
		Fn &&fn) && {
		return move(*this).schedule_on(target).then(forward<Fn>(fn));
	}
	// materialize resolved outcome as a Task (always allocates one control block)
	[[nodiscard]] root::Task<T> into_task(
		std::source_location loc = std::source_location::current()) && {
		auto [task, src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false}, loc);
		if (outcome_.is_success()) {
			if constexpr (same_as<T, void>) {
				auto _ = src.try_set_value(root::Success<T>{});
			} else {
				auto _ = src.try_set_value(root::Success<T>{move(outcome_).success().value});
			}
		} else if (outcome_.is_failure()) {
			auto _ = src.try_set_exception(outcome_.failure().error);
		} else {
			auto _ = src.try_set_cancelled(root::cancel_reason_errc(outcome_.cancelled().reason));
		}
		return move(task);
	}
};
template<root::work_value T>
struct ChainAwaiter {
	Chain<T> chain_;
	[[nodiscard]] bool await_ready() const noexcept { return true; }
	void await_suspend(
		std::coroutine_handle<>) const noexcept {}
	decltype(auto) await_resume() {
		auto out = move(chain_).release_outcome();
		if (out.is_success()) {
			if constexpr (!same_as<T, void>) {
				return move(out).success().value;
			} else {
				return;
			}
		}
		if (out.is_failure()) {
			rethrow_exception(move(out).failure().error);
		}
		throw root::CancelledError{out.cancelled().reason};
	}
};
template<root::work_value T>
ChainAwaiter<T> Chain<T>::operator co_await() && noexcept {
	return ChainAwaiter<T>{move(*this)};
}
template<root::work_value T>
[[nodiscard]] Chain<T> from_task(
	root::Task<T> &&task) {
	return Chain<T>{root::join(move(task)), CarrierKind::task};
}
template<root::work_value T, root::progress_capability Owner>
[[nodiscard]] Chain<T> from_posted(
	Owner &owner,
	root::Posted<T> &&posted) {
	return Chain<T>{root::join(owner, move(posted)), CarrierKind::posted};
}
template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] Chain<T> from_operation(
	Driver &driver,
	root::Operation<T> &&op) {
	return Chain<T>{root::join(driver, move(op)), CarrierKind::operation};
}
template<root::work_value T, root::progress_capability Owner>
[[nodiscard]] Chain<T> hop_to_posted(
	Owner &owner,
	Chain<T> &&chain) noexcept {
	return Chain<T>{move(chain).release_outcome(), CarrierKind::posted, root::capability_id(owner)};
}
template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] Chain<T> hop_to_operation(
	Driver &driver,
	Chain<T> &&chain) noexcept {
	return Chain<T>{move(chain).release_outcome(), CarrierKind::operation, root::capability_id(driver)};
}
template<root::work_value T>
[[nodiscard]] Chain<T> hop_to_task(
	Chain<T> &&chain) noexcept {
	return Chain<T>{move(chain).release_outcome(), CarrierKind::task};
}
template<root::work_value T>
[[nodiscard]] Chain<T> unbind(
	Chain<T> &&chain) noexcept {
	auto kind = chain.kind();
	return Chain<T>{move(chain).release_outcome(), kind};
}
template<root::work_value T>
[[nodiscard]] root::Task<T> into_ready_task(
	Chain<T> &&chain) {
	auto [task, src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false});
	auto out = move(chain).release_outcome();
	if (out.is_success()) {
		if constexpr (same_as<T, void>) {
			auto _ = src.try_set_value(root::Success<void>{});
		} else {
			auto _ = src.try_set_value(root::Success<T>{move(out).success().value});
		}
	} else if (out.is_failure()) {
		auto _ = src.try_set_exception(move(out).failure().error);
	} else {
		auto _ = src.try_set_cancelled(root::cancel_reason_errc(out.cancelled().reason));
	}
	return move(task);
}
template<root::progress_capability Cap, root::work_value T>
void verify_hop(
	Cap const &cap,
	Chain<T> const &chain) {
	auto const bound = chain.bound_capability();
	if (bound.address && bound != root::capability_id(cap)) {
		throw HopCapabilityError{};
	}
}
template<root::work_value T, class Fn>
	requires(!same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto map(
	Chain<T> &&chain,
	Fn &&fn) -> Chain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
	auto kind = chain.kind();
	return Chain<U>{detail::map_outcome(move(chain).release_outcome(), forward<Fn>(fn)), kind};
}
template<root::work_value T, class Fn>
	requires(!same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto then(
	Chain<T> &&chain,
	Fn &&fn) -> Chain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	return map(move(chain), forward<Fn>(fn));
}
template<root::work_value A, root::work_value B>
	requires(!same_as<A, void> && !same_as<B, void>)
[[nodiscard]] auto when_all(
	Chain<A> &&a,
	Chain<B> &&b) noexcept -> Chain<Tup<A, B>> {
	using T = Tup<A, B>;
	auto out_a = move(a).release_outcome();
	auto out_b = move(b).release_outcome();

	if (out_a.is_failure() && out_b.is_failure()) {
		auto agg = make_exception_ptr(
			AggregateError{
				{move(out_a).failure().error, move(out_b).failure().error}
        });
		return Chain<T>{root::Outcome<T>{root::Failure{agg}}, CarrierKind::task};
	}
	if (out_a.is_failure()) {
		return Chain<T>{root::Outcome<T>{move(out_a).failure()}, CarrierKind::task};
	}
	if (out_b.is_failure()) {
		return Chain<T>{root::Outcome<T>{move(out_b).failure()}, CarrierKind::task};
	}
	if (out_a.is_cancelled()) {
		return Chain<T>{root::Outcome<T>{move(out_a).cancelled()}, CarrierKind::task};
	}
	if (out_b.is_cancelled()) {
		return Chain<T>{root::Outcome<T>{move(out_b).cancelled()}, CarrierKind::task};
	}

	return Chain<T>{
		root::Outcome<T>{root::Success<T>{T{move(out_a).success().value, move(out_b).success().value}}},
		CarrierKind::task};
}
template<root::work_value A, root::work_value B>
	requires(!same_as<A, void> && !same_as<B, void>)
[[nodiscard]] auto when_all_fast_fail(
	Chain<A> &&a,
	Chain<B> &&b) noexcept -> Chain<Tup<A, B>> {
	// TODO(phase-6): wire cancel-sibling hook once 5c async path lands;
	// currently identical to when_all
	return when_all(move(a), move(b));
}
template<root::work_value T>
	requires(!same_as<T, void>)
[[nodiscard]] Chain<T> race(
	Chain<T> &&a,
	Chain<T> &&b) noexcept {
	auto kind_a = a.kind();
	auto cap_a = a.bound_capability();
	auto kind_b = b.kind();
	auto cap_b = b.bound_capability();
	auto out_a = move(a).release_outcome();
	auto out_b = move(b).release_outcome();

	if (out_a.is_success()) {
		return Chain<T>{move(out_a), kind_a, cap_a};
	}
	if (out_b.is_success()) {
		return Chain<T>{move(out_b), kind_b, cap_b};
	}
	if (out_a.is_failure()) {
		return Chain<T>{move(out_a), kind_a, cap_a};
	}
	if (out_b.is_failure()) {
		return Chain<T>{move(out_b), kind_b, cap_b};
	}
	return Chain<T>{move(out_a), kind_a, cap_a};
}

} // namespace conflux::work::carrier
