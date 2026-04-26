module;

export module conflux.work.carrier.model_b;

import std;
import conflux.work.root;
import conflux.work.carrier.flags;

namespace conflux::work::carrier::model_b::detail {

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

} // namespace conflux::work::carrier::model_b::detail

export namespace conflux::work::carrier::model_b {

template<root::work_value T>
class TaskChain {
	root::Outcome<T> outcome_;

public:
	TaskChain() = delete;

	explicit TaskChain(
		root::Outcome<T> outcome) noexcept
		: outcome_{std::move(outcome)} {}

	TaskChain(TaskChain &&) noexcept = default;
	TaskChain &operator =(TaskChain &&) noexcept = default;
	TaskChain(TaskChain const &) = delete;
	TaskChain &operator =(TaskChain const &) = delete;

	[[nodiscard]] root::Outcome<T> release_outcome() && noexcept { return std::move(outcome_); }
};

template<root::work_value T>
class PostedChain {
	root::Outcome<T> outcome_;

public:
	PostedChain() = delete;

	explicit PostedChain(
		root::Outcome<T> outcome) noexcept
		: outcome_{std::move(outcome)} {}

	PostedChain(PostedChain &&) noexcept = default;
	PostedChain &operator =(PostedChain &&) noexcept = default;
	PostedChain(PostedChain const &) = delete;
	PostedChain &operator =(PostedChain const &) = delete;

	[[nodiscard]] root::Outcome<T> release_outcome() && noexcept { return std::move(outcome_); }
};

template<root::work_value T>
class OperationChain {
	root::Outcome<T> outcome_;

public:
	OperationChain() = delete;

	explicit OperationChain(
		root::Outcome<T> outcome) noexcept
		: outcome_{std::move(outcome)} {}

	OperationChain(OperationChain &&) noexcept = default;
	OperationChain &operator =(OperationChain &&) noexcept = default;
	OperationChain(OperationChain const &) = delete;
	OperationChain &operator =(OperationChain const &) = delete;

	[[nodiscard]] root::Outcome<T> release_outcome() && noexcept { return std::move(outcome_); }
};

template<root::work_value T>
[[nodiscard]] TaskChain<T> from_task(
	root::Task<T> &&task) {
	return TaskChain<T>{root::join(std::move(task))};
}

template<root::work_value T, root::progress_capability Owner>
[[nodiscard]] PostedChain<T> from_posted(
	Owner &owner,
	root::Posted<T> &&posted) {
	return PostedChain<T>{root::join(owner, std::move(posted))};
}

template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] OperationChain<T> from_operation(
	Driver &driver,
	root::Operation<T> &&op) {
	return OperationChain<T>{root::join(driver, std::move(op))};
}

template<root::work_value T, root::progress_capability Owner>
[[nodiscard]] PostedChain<T> hop_to_posted(
	Owner &,
	TaskChain<T> &&chain) noexcept {
	return PostedChain<T>{std::move(chain).release_outcome()};
}

template<root::work_value T, root::progress_capability Driver>
[[nodiscard]] OperationChain<T> hop_to_operation(
	Driver &,
	PostedChain<T> &&chain) noexcept {
	return OperationChain<T>{std::move(chain).release_outcome()};
}

template<root::work_value T, class Fn>
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto map(
	TaskChain<T> &&chain,
	Fn &&fn) -> TaskChain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
	return TaskChain<U>{detail::map_outcome(std::move(chain).release_outcome(), std::forward<Fn>(fn))};
}

template<root::work_value T, class Fn>
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto map(
	PostedChain<T> &&chain,
	Fn &&fn) -> PostedChain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
	return PostedChain<U>{detail::map_outcome(std::move(chain).release_outcome(), std::forward<Fn>(fn))};
}

template<root::work_value T, class Fn>
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto map(
	OperationChain<T> &&chain,
	Fn &&fn) -> OperationChain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	using U = std::remove_cvref_t<std::invoke_result_t<Fn &, T>>;
	return OperationChain<U>{detail::map_outcome(std::move(chain).release_outcome(), std::forward<Fn>(fn))};
}

template<root::work_value T, class Fn>
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto then(
	TaskChain<T> &&chain,
	Fn &&fn) -> TaskChain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	return map(std::move(chain), std::forward<Fn>(fn));
}

template<root::work_value T, class Fn>
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto then(
	PostedChain<T> &&chain,
	Fn &&fn) -> PostedChain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	return map(std::move(chain), std::forward<Fn>(fn));
}

template<root::work_value T, class Fn>
	requires(!std::same_as<T, void>)
		 && std::invocable<Fn &, T>
		 && root::work_value<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>>
[[nodiscard]] auto then(
	OperationChain<T> &&chain,
	Fn &&fn) -> OperationChain<std::remove_cvref_t<std::invoke_result_t<Fn &, T>>> {
	return map(std::move(chain), std::forward<Fn>(fn));
}

template<root::work_value A, root::work_value B>
	requires(!std::same_as<A, void> && !std::same_as<B, void>)
[[nodiscard]] auto when_all(
	TaskChain<A> &&a,
	TaskChain<B> &&b) noexcept -> TaskChain<std::tuple<A, B>> {
	using T = std::tuple<A, B>;
	auto out_a = std::move(a).release_outcome();
	auto out_b = std::move(b).release_outcome();

	if (out_a.is_failure()) {
		return TaskChain<T>{root::Outcome<T>{std::move(out_a).failure()}};
	}
	if (out_b.is_failure()) {
		return TaskChain<T>{root::Outcome<T>{std::move(out_b).failure()}};
	}
	if (out_a.is_cancelled()) {
		return TaskChain<T>{root::Outcome<T>{std::move(out_a).cancelled()}};
	}
	if (out_b.is_cancelled()) {
		return TaskChain<T>{root::Outcome<T>{std::move(out_b).cancelled()}};
	}

	return TaskChain<T>{
		root::Outcome<T>{root::Success<T>{T{std::move(out_a).success().value, std::move(out_b).success().value}}}};
}

} // namespace conflux::work::carrier::model_b
