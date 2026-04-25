module;

export module conflux.work.carrier.model_a;

import std;
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

enum class CarrierKind : std::uint8_t {
	task,
	posted,
	operation,
};

template<root::work_value T>
class Chain {
	root::Outcome<T> outcome_;
	CarrierKind kind_ = CarrierKind::task;

public:
	Chain() = delete;

	Chain(
		root::Outcome<T> outcome,
		CarrierKind kind) noexcept
		: outcome_{std::move(outcome)}
		, kind_{kind} {}

	Chain(Chain &&) noexcept = default;
	Chain &operator =(Chain &&) noexcept = default;
	Chain(Chain const &) = delete;
	Chain &operator =(Chain const &) = delete;

	[[nodiscard]] CarrierKind kind() const noexcept { return kind_; }

	[[nodiscard]] root::Outcome<T> release_outcome() && noexcept { return std::move(outcome_); }
};

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
[[nodiscard]] Chain<T> bridge_to_posted(
	Owner &,
	Chain<T> &&chain) noexcept {
	return Chain<T>{std::move(chain).release_outcome(), CarrierKind::posted};
}

template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] Chain<T> bridge_to_operation(
	Driver &,
	Chain<T> &&chain) noexcept {
	return Chain<T>{std::move(chain).release_outcome(), CarrierKind::operation};
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
	Chain<B> &&b) noexcept -> Chain<std::tuple<A, B>> {
	using T = std::tuple<A, B>;
	auto out_a = std::move(a).release_outcome();
	auto out_b = std::move(b).release_outcome();

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
	Chain<B> &&b) noexcept -> Chain<std::tuple<A, B>> {
	// In eager context both chains are already resolved; semantically identical
	// to when_all. API contract: in async context failure triggers best-effort
	// sibling cancellation before waiting for the loser to complete.
	return when_all(std::move(a), std::move(b));
}

template<root::work_value T>
	requires(!std::same_as<T, void>)
[[nodiscard]] Chain<T> race(
	Chain<T> &&a,
	Chain<T> &&b) noexcept {
	auto kind_a = a.kind();
	auto kind_b = b.kind();
	auto out_a = std::move(a).release_outcome();
	auto out_b = std::move(b).release_outcome();

	if (out_a.is_success()) {
		return Chain<T>{std::move(out_a), kind_a};
	}
	if (out_b.is_success()) {
		return Chain<T>{std::move(out_b), kind_b};
	}
	if (out_a.is_failure()) {
		return Chain<T>{std::move(out_a), kind_a};
	}
	if (out_b.is_failure()) {
		return Chain<T>{std::move(out_b), kind_b};
	}
	return Chain<T>{std::move(out_a), kind_a};
}

} // namespace conflux::work::carrier::model_a
