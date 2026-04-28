module;

export module conflux.work.carrier.model_a;

import std;
import conflux.types;
import conflux.work.root;
import conflux.work.carrier.flags;

namespace conflux::work::carrier::model_a::detail {

template<root::work_value T, class Fn>
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
auto map_outcome(
	root::Outcome<T> out,
	Fn &&fn) -> root::Outcome<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
	if (out.is_failure()) {
		return root::Outcome<U>{std::move(out).failure()};
	}
	if (out.is_cancelled()) {
		return root::Outcome<U>{std::move(out).cancelled()};
	}
	try {
		if constexpr (std::same_as<U, void>) {
			std::invoke(std::forward<Fn>(fn), std::move(out).success().value);
			return root::Outcome<U>{root::Success<U>{}};
		} else {
			auto result = std::invoke(std::forward<Fn>(fn), std::move(out).success().value);
			return root::Outcome<U>{root::Success<U>{std::move(result)}};
		}
	} catch (...) { return root::Outcome<U>{root::Failure{std::current_exception()}}; }
}

} // namespace conflux::work::carrier::model_a::detail

export namespace conflux::work::carrier::model_a {

template<root::work_value T>
struct ChainAwaiter;

enum class CarrierKind : std::uint8_t {
	task,
	posted,
	operation,
};

class HopCapabilityError : public root::JoinContextError {
public:
	HopCapabilityError()
		: JoinContextError{"carrier: hop capability mismatch", root::JoinContextReason::hop_capability_mismatch} {}
};

class AggregateError : public root::WorkError {
	V<std::exception_ptr> causes_;

public:
	explicit AggregateError(
		V<std::exception_ptr> causes)
		: WorkError{"carrier: multiple failures"}
		, causes_{std::move(causes)} {}

	[[nodiscard]] V<std::exception_ptr> causes_owned() const { return causes_; }

	[[nodiscard("span lifetime bound to *this — moves invalidate")]] std::span<std::exception_ptr const>
	causes_view() const noexcept {
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
		: outcome_{std::move(outcome)}
		, kind_{kind} {}

	Chain(
		root::Outcome<T> outcome,
		CarrierKind kind,
		root::CapabilityId cap) noexcept
		: outcome_{std::move(outcome)}
		, kind_{kind}
		, bound_cap_{cap} {}

	Chain(Chain &&) noexcept = default;
	Chain &operator =(Chain &&) noexcept = default;
	Chain(Chain const &) = delete;
	Chain &operator =(Chain const &) = delete;

	[[nodiscard]] CarrierKind kind() const noexcept { return kind_; }

	[[nodiscard]] root::CapabilityId bound_capability() const noexcept { return bound_cap_; }

	[[nodiscard]] root::Outcome<T> release_outcome() && noexcept { return std::move(outcome_); }

	[[nodiscard]] ChainAwaiter<T> operator co_await() && noexcept;
};

template<root::work_value T>
struct ChainAwaiter {
	Chain<T> chain_;

	[[nodiscard]] bool await_ready() const noexcept { return true; }
	void await_suspend(
		std::coroutine_handle<>) const noexcept {}

	decltype(auto) await_resume() {
		auto out = std::move(chain_).release_outcome();
		if (out.is_success()) {
			if constexpr (!std::same_as<T, void>) {
				return std::move(out).success().value;
			} else {
				return;
			}
		}
		if (out.is_failure()) {
			std::rethrow_exception(std::move(out).failure().error);
		}
		throw root::CancelledError{out.cancelled().reason};
	}
};

template<root::work_value T>
ChainAwaiter<T> Chain<T>::operator co_await() && noexcept {
	return ChainAwaiter<T>{std::move(*this)};
}

template<root::work_value T>
[[nodiscard]] Chain<T> from_task(
	root::Task<T> &&task) {
	return Chain<T>{root::join(std::move(task)), CarrierKind::task};
}

template<root::work_value T, root::progress_capability Owner>
[[nodiscard]] Chain<T> from_posted(
	Owner &owner,
	root::Posted<T> &&posted) {
	return Chain<T>{root::join(owner, std::move(posted)), CarrierKind::posted};
}

template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] Chain<T> from_operation(
	Driver &driver,
	root::Operation<T> &&op) {
	return Chain<T>{root::join(driver, std::move(op)), CarrierKind::operation};
}

template<root::work_value T, root::progress_capability Owner>
[[nodiscard]] Chain<T> hop_to_posted(
	Owner &owner,
	Chain<T> &&chain) noexcept {
	return Chain<T>{std::move(chain).release_outcome(), CarrierKind::posted, root::capability_id(owner)};
}

template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] Chain<T> hop_to_operation(
	Driver &driver,
	Chain<T> &&chain) noexcept {
	return Chain<T>{std::move(chain).release_outcome(), CarrierKind::operation, root::capability_id(driver)};
}

template<root::work_value T>
[[nodiscard]] Chain<T> hop_to_task(
	Chain<T> &&chain) noexcept {
	return Chain<T>{std::move(chain).release_outcome(), CarrierKind::task};
}

template<root::work_value T>
[[nodiscard]] Chain<T> unbind(
	Chain<T> &&chain) noexcept {
	auto kind = chain.kind();
	return Chain<T>{std::move(chain).release_outcome(), kind};
}

template<root::work_value T>
[[nodiscard]] root::Task<T> into_ready_task(
	Chain<T> &&chain) {
	auto [task, src] = root::make_task_source<T>(root::SubmitOptions{.enable_cancellation = false});
	auto out = std::move(chain).release_outcome();
	if (out.is_success()) {
		if constexpr (std::same_as<T, void>) {
			(void)src.commit_success(root::Success<void>{});
		} else {
			(void)src.commit_success(root::Success<T>{std::move(out).success().value});
		}
	} else if (out.is_failure()) {
		(void)src.commit_failure(std::move(out).failure().error);
	} else {
		(void)src.commit_cancelled(out.cancelled().reason);
	}
	return std::move(task);
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
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto map(
	Chain<T> &&chain,
	Fn &&fn) -> Chain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
	auto kind = chain.kind();
	return Chain<U>{detail::map_outcome(std::move(chain).release_outcome(), std::forward<Fn>(fn)), kind};
}

template<root::work_value T, class Fn>
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto then(
	Chain<T> &&chain,
	Fn &&fn) -> Chain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	return map(std::move(chain), std::forward<Fn>(fn));
}

template<root::work_value A, root::work_value B>
	requires(!std::same_as<A, void> && !std::same_as<B, void>)
[[nodiscard]] auto when_all(
	Chain<A> &&a,
	Chain<B> &&b) noexcept -> Chain<Tup<A, B>> {
	using T = Tup<A, B>;
	auto out_a = std::move(a).release_outcome();
	auto out_b = std::move(b).release_outcome();

	if (out_a.is_failure() && out_b.is_failure()) {
		auto agg = std::make_exception_ptr(
			AggregateError{
				{std::move(out_a).failure().error, std::move(out_b).failure().error}
        });
		return Chain<T>{root::Outcome<T>{root::Failure{agg}}, CarrierKind::task};
	}
	if (out_a.is_failure()) {
		return Chain<T>{root::Outcome<T>{std::move(out_a).failure()}, CarrierKind::task};
	}
	if (out_b.is_failure()) {
		return Chain<T>{root::Outcome<T>{std::move(out_b).failure()}, CarrierKind::task};
	}
	if (out_a.is_cancelled()) {
		return Chain<T>{root::Outcome<T>{std::move(out_a).cancelled()}, CarrierKind::task};
	}
	if (out_b.is_cancelled()) {
		return Chain<T>{root::Outcome<T>{std::move(out_b).cancelled()}, CarrierKind::task};
	}

	return Chain<T>{
		root::Outcome<T>{root::Success<T>{T{std::move(out_a).success().value, std::move(out_b).success().value}}},
		CarrierKind::task};
}

template<root::work_value A, root::work_value B>
	requires(!std::same_as<A, void> && !std::same_as<B, void>)
[[nodiscard]] auto when_all_fast_fail(
	Chain<A> &&a,
	Chain<B> &&b) noexcept -> Chain<Tup<A, B>> {
	// TODO(phase-6): wire cancel-sibling hook once 5c async path lands;
	// currently identical to when_all
	return when_all(std::move(a), std::move(b));
}

template<root::work_value T>
	requires(!std::same_as<T, void>)
[[nodiscard]] Chain<T> race(
	Chain<T> &&a,
	Chain<T> &&b) noexcept {
	auto kind_a = a.kind();
	auto cap_a = a.bound_capability();
	auto kind_b = b.kind();
	auto cap_b = b.bound_capability();
	auto out_a = std::move(a).release_outcome();
	auto out_b = std::move(b).release_outcome();

	if (out_a.is_success()) {
		return Chain<T>{std::move(out_a), kind_a, cap_a};
	}
	if (out_b.is_success()) {
		return Chain<T>{std::move(out_b), kind_b, cap_b};
	}
	if (out_a.is_failure()) {
		return Chain<T>{std::move(out_a), kind_a, cap_a};
	}
	if (out_b.is_failure()) {
		return Chain<T>{std::move(out_b), kind_b, cap_b};
	}
	return Chain<T>{std::move(out_a), kind_a, cap_a};
}

} // namespace conflux::work::carrier::model_a
