export module conflux.net.app.response;

import std;
import conflux.net.http.response;
import conflux.net.app.types;
#if CONFLUX_HAS_JSON
import conflux.net.http.native_json;
#endif

export namespace conflux::http {

template<class T>
concept ExpectedHttpProblem = requires(T value) {
	typename T::value_type;
	typename T::error_type;
	requires std::same_as<typename T::error_type, Problem>;
	{ static_cast<bool>(value) } -> std::same_as<bool>;
	{ *value };
	{ value.error() } -> std::same_as<Problem &>;
};

template<class>
inline constexpr bool kDependentFalse = false;

[[nodiscard]] inline Response into_response(
	Response response) {
	return response;
}

[[nodiscard]] inline Response into_response(
	Problem problem) {
	return std::move(problem.response);
}

[[nodiscard]] inline Response into_response(
	Created created) {
	return std::move(created.response);
}

template<class T>
[[nodiscard]] Response into_response(
	Json<T> const &body) {
#if CONFLUX_HAS_JSON
	if constexpr (requires(T const &value) {
					  { codec::json::response_or_internal_error(value) } -> std::same_as<Response>;
				  }) {
		return codec::json::response_or_internal_error(body.value);
	} else {
		static_assert(
			kDependentFalse<T>,
			"http::Json<T> responses require T to be serializable; add JsonCodec<T>, JsonMembers<T>, or reflection "
			"JSON support for T");
	}
#else
	(void)body;
	return Response::internal_error("JSON support is not enabled");
#endif
}

template<class T>
[[nodiscard]] Response into_response(
	Json<T> &&body) {
#if CONFLUX_HAS_JSON
	if constexpr (requires(T const &value) {
					  { codec::json::response_or_internal_error(value) } -> std::same_as<Response>;
				  }) {
		return codec::json::response_or_internal_error(body.value);
	} else {
		static_assert(
			kDependentFalse<T>,
			"http::Json<T> responses require T to be serializable; add JsonCodec<T>, JsonMembers<T>, or reflection "
			"JSON support for T");
	}
#else
	(void)body;
	return Response::internal_error("JSON support is not enabled");
#endif
}

template<class T>
[[nodiscard]] Response into_response(
	T &&result)
	requires ExpectedHttpProblem<std::remove_cvref_t<T>>
{
	if (result) {
		return into_response(*std::forward<T>(result));
	}
	return into_response(std::forward<T>(result).error());
}

template<class T>
concept IntoResponse = requires(T &&value) {
	{ into_response(std::forward<T>(value)) } -> std::same_as<Response>;
};

template<class T>
concept RawStringResponse = std::same_as<std::remove_cvref_t<T>, std::string>
						 || std::same_as<std::remove_cvref_t<T>, std::string_view>
						 || std::same_as<std::remove_cvref_t<T>, char const *>;

template<class F>
concept NullaryRawStringHandler = requires(F &fn) {
	{ fn() } -> RawStringResponse;
};

} // namespace conflux::http
