export module conflux.net.app.traits;

import std;
import conflux.net.app.types;
import conflux.net.http.server_types;

export namespace conflux::http { namespace detail {

template<class T>
struct FunctionArgs;

template<class R, class... Args>
struct FunctionArgs<R (*)(Args...)> {
	using type = std::tuple<Args...>;
};

template<class C, class R, class... Args>
struct FunctionArgs<R (C::*)(Args...)> {
	using type = std::tuple<Args...>;
};

template<class C, class R, class... Args>
struct FunctionArgs<R (C::*)(Args...) const> {
	using type = std::tuple<Args...>;
};

template<class F>
concept HasFunctionArgs = requires { typename FunctionArgs<decltype(&std::remove_reference_t<F>::operator ())>::type; };

template<class F>
struct CallableArgs {
	using type = FunctionArgs<decltype(&std::remove_reference_t<F>::operator ())>::type;
};

template<class R, class... Args>
struct CallableArgs<R (*)(Args...)> {
	using type = std::tuple<Args...>;
};

template<class T>
struct StateType {};

template<class T>
struct StateType<State<T>> {
	using type = T;
};

template<class T>
concept StateArg = requires { typename StateType<std::remove_cvref_t<T>>::type; };

template<class T>
struct JsonType {};

template<class T>
struct JsonType<Json<T>> {
	using type = T;
};

template<class T>
concept JsonArg = requires { typename JsonType<std::remove_cvref_t<T>>::type; };

template<class T>
struct PathType {};

template<FixedString Name, class T>
struct PathType<Path<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept PathArg = requires { typename PathType<std::remove_cvref_t<T>>::type; };

template<class T>
struct PathAtType {};

template<std::size_t Index, class T>
struct PathAtType<PathAt<Index, T>> {
	using type = T;
	static constexpr std::size_t index = Index;
};

template<class T>
concept PathAtArg = requires { typename PathAtType<std::remove_cvref_t<T>>::type; };

template<class T>
struct QueryType {};

template<FixedString Name, class T>
struct QueryType<Query<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept QueryArg = requires { typename QueryType<std::remove_cvref_t<T>>::type; };

template<class T>
struct HeaderType {};

template<FixedString Name, class T>
struct HeaderType<Header<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept HeaderArg = requires { typename HeaderType<std::remove_cvref_t<T>>::type; };

template<class T>
struct CookieType {};

template<FixedString Name, class T>
struct CookieType<Cookie<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept CookieArg = requires { typename CookieType<std::remove_cvref_t<T>>::type; };

template<class T>
struct FormType {};

template<FixedString Name, class T>
struct FormType<Form<Name, T>> {
	using type = T;
	static constexpr auto name = Name;
};

template<class T>
concept FormArg = requires { typename FormType<std::remove_cvref_t<T>>::type; };

#if CONFLUX_HAS_JSON
template<class T>
struct QueryParamsType {};

template<class T>
struct QueryParamsType<QueryParams<T>> {
	using type = T;
};

template<class T>
concept QueryParamsArg = requires { typename QueryParamsType<std::remove_cvref_t<T>>::type; };

template<class T>
struct FormParamsType {};

template<class T>
struct FormParamsType<FormParams<T>> {
	using type = T;
};

template<class T>
concept FormParamsArg = requires { typename FormParamsType<std::remove_cvref_t<T>>::type; };
#endif

template<class Arg>
concept RequestViewArg = std::same_as<std::remove_cvref_t<Arg>, RequestView>;

template<class Arg>
concept RequestArg = std::same_as<std::remove_cvref_t<Arg>, Request>;

template<class Arg>
concept BodyTextArg = std::same_as<std::remove_cvref_t<Arg>, BodyText>;

template<class Arg>
concept BodyBytesArg = std::same_as<std::remove_cvref_t<Arg>, BodyBytes>;

template<class Arg>
concept OwnedBodyBytesArg = std::same_as<std::remove_cvref_t<Arg>, OwnedBodyBytes>;

#if CONFLUX_HAS_JSON
template<class Arg>
concept JsonDocumentArg = std::same_as<std::remove_cvref_t<Arg>, JsonDocument>;

template<class Arg>
concept JsonPatchArg = std::same_as<std::remove_cvref_t<Arg>, JsonPatch>;

template<class Arg>
concept MergePatchArg = std::same_as<std::remove_cvref_t<Arg>, MergePatch>;
#endif

template<class Arg>
concept MultipartArg = std::same_as<std::remove_cvref_t<Arg>, Multipart>;

template<class Arg>
concept RequestIdArg = std::same_as<std::remove_cvref_t<Arg>, RequestId>;

template<class Arg>
concept ConnectionInfoArg = std::same_as<std::remove_cvref_t<Arg>, ConnectionInfo>;

template<class Arg>
concept TraceContextArg = std::same_as<std::remove_cvref_t<Arg>, TraceContext>;

template<class Arg>
concept BearerArg = std::same_as<std::remove_cvref_t<Arg>, Bearer>;

template<class Arg>
concept RequiredBearerArg = std::same_as<std::remove_cvref_t<Arg>, RequiredBearer>;

template<class Arg>
concept OptionalBearerArg = std::same_as<std::remove_cvref_t<Arg>, OptionalBearer>;

template<class Arg>
concept BasicAuthArg = std::same_as<std::remove_cvref_t<Arg>, BasicAuth>;

template<class Arg>
concept InlinePathArg = (std::signed_integral<std::remove_cvref_t<Arg>> && sizeof(std::remove_cvref_t<Arg>) == 8)
					 || (std::unsigned_integral<std::remove_cvref_t<Arg>> && sizeof(std::remove_cvref_t<Arg>) == 8)
					 || (std::signed_integral<std::remove_cvref_t<Arg>> && sizeof(std::remove_cvref_t<Arg>) == 4)
					 || (std::unsigned_integral<std::remove_cvref_t<Arg>> && sizeof(std::remove_cvref_t<Arg>) == 4)
					 || std::same_as<std::remove_cvref_t<Arg>, std::string_view>
					 || std::same_as<std::remove_cvref_t<Arg>, std::string>;

template<class Arg, class Body>
concept RawJsonBodyArg = std::same_as<std::remove_cvref_t<Arg>, std::remove_cvref_t<Body>>;

template<class T>
struct ExpectedValueType {};

template<class T, class E>
struct ExpectedValueType<std::expected<T, E>> {
	using type = T;
};

template<class T>
struct ResponseMetadataType {
	using type = std::remove_cvref_t<T>;
};

template<class T>
	requires requires { typename ExpectedValueType<std::remove_cvref_t<T>>::type; }
struct ResponseMetadataType<T> {
	using type = typename ExpectedValueType<std::remove_cvref_t<T>>::type;
};

template<class T>
struct ReturnsProblemResponse : std::false_type {};

template<>
struct ReturnsProblemResponse<Problem> : std::true_type {};

template<class T>
struct ReturnsProblemResponse<std::expected<T, Problem>> : std::true_type {};

template<class Args, std::size_t... Is>
consteval bool has_state_arg_impl(
	std::index_sequence<Is...>) {
	return (
		false
		|| ...
		|| (StateArg<std::tuple_element_t<Is, Args>>
			|| PathArg<std::tuple_element_t<Is, Args>>
			|| PathAtArg<std::tuple_element_t<Is, Args>>
			|| QueryArg<std::tuple_element_t<Is, Args>>
			|| HeaderArg<std::tuple_element_t<Is, Args>>
			|| CookieArg<std::tuple_element_t<Is, Args>>
			|| FormArg<std::tuple_element_t<Is, Args>>
#if CONFLUX_HAS_JSON
			|| QueryParamsArg<std::tuple_element_t<Is, Args>>
			|| FormParamsArg<std::tuple_element_t<Is, Args>>
			|| JsonArg<std::tuple_element_t<Is, Args>>
			|| JsonPatchArg<std::tuple_element_t<Is, Args>>
			|| MergePatchArg<std::tuple_element_t<Is, Args>>
#endif
			|| BodyTextArg<std::tuple_element_t<Is, Args>>
			|| BodyBytesArg<std::tuple_element_t<Is, Args>>
			|| OwnedBodyBytesArg<std::tuple_element_t<Is, Args>>
#if CONFLUX_HAS_JSON
			|| JsonDocumentArg<std::tuple_element_t<Is, Args>>
			|| JsonPatchArg<std::tuple_element_t<Is, Args>>
			|| MergePatchArg<std::tuple_element_t<Is, Args>>
#endif
			|| MultipartArg<std::tuple_element_t<Is, Args>>
			|| RequestIdArg<std::tuple_element_t<Is, Args>>
			|| ConnectionInfoArg<std::tuple_element_t<Is, Args>>
			|| TraceContextArg<std::tuple_element_t<Is, Args>>
			|| BearerArg<std::tuple_element_t<Is, Args>>
			|| RequiredBearerArg<std::tuple_element_t<Is, Args>>
			|| OptionalBearerArg<std::tuple_element_t<Is, Args>>
			|| BasicAuthArg<std::tuple_element_t<Is, Args>>));
}

template<class Args>
consteval bool has_state_arg() {
	return has_state_arg_impl<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
}

template<class Args, std::size_t Index, std::size_t... Is>
consteval std::size_t inline_path_arg_index_impl(
	std::index_sequence<Is...>) {
	return (
		std::size_t{0}
		+ ...
		+ ((Is < Index && InlinePathArg<std::tuple_element_t<Is, Args>>) ? std::size_t{1} : std::size_t{0}));
}

template<class Args, std::size_t Index>
consteval std::size_t inline_path_arg_index() {
	return inline_path_arg_index_impl<Args, Index>(std::make_index_sequence<Index>{});
}

}} // namespace conflux::http::detail
