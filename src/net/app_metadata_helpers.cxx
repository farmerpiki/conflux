export module conflux.net.app.metadata_helpers;

import std;
import conflux.net.app.traits;

export namespace conflux::http::detail {

template<class Args, std::size_t... Is>
void append_required_states(
	std::vector<std::type_index> &out,
	std::index_sequence<Is...>) {
	(
		[&] {
			using Arg = std::tuple_element_t<Is, Args>;
			if constexpr (StateArg<Arg>) {
				using StateValue = typename StateType<std::remove_cvref_t<Arg>>::type;
				out.push_back(std::type_index{typeid(StateValue)});
			}
		}(),
		...);
}

template<class Arg>
[[nodiscard]] std::string extractor_name() {
	using Clean = std::remove_cvref_t<Arg>;
	if constexpr (StateArg<Clean>) {
		return "State";
	} else if constexpr (PathArg<Clean>) {
		return std::format("Path<{}>", PathType<Clean>::name.view());
	} else if constexpr (PathAtArg<Clean>) {
		return std::format("PathAt<{}>", PathAtType<Clean>::index);
	} else if constexpr (QueryArg<Clean>) {
		return std::format("Query<{}>", QueryType<Clean>::name.view());
	} else if constexpr (HeaderArg<Clean>) {
		return std::format("Header<{}>", HeaderType<Clean>::name.view());
	} else if constexpr (CookieArg<Clean>) {
		return std::format("Cookie<{}>", CookieType<Clean>::name.view());
	} else if constexpr (FormArg<Clean>) {
		return std::format("Form<{}>", FormType<Clean>::name.view());
#if CONFLUX_HAS_JSON
	} else if constexpr (QueryParamsArg<Clean>) {
		return "QueryParams";
	} else if constexpr (FormParamsArg<Clean>) {
		return "FormParams";
#endif
	} else if constexpr (BodyTextArg<Clean>) {
		return "BodyText";
	} else if constexpr (BodyBytesArg<Clean>) {
		return "BodyBytes";
	} else if constexpr (OwnedBodyBytesArg<Clean>) {
		return "OwnedBodyBytes";
#if CONFLUX_HAS_JSON
	} else if constexpr (JsonDocumentArg<Clean>) {
		return "JsonDocument";
	} else if constexpr (JsonPatchArg<Clean>) {
		return "JsonPatch";
	} else if constexpr (MergePatchArg<Clean>) {
		return "MergePatch";
#endif
	} else if constexpr (MultipartArg<Clean>) {
		return "Multipart";
	} else if constexpr (RequestIdArg<Clean>) {
		return "RequestId";
	} else if constexpr (ConnectionInfoArg<Clean>) {
		return "ConnectionInfo";
	} else if constexpr (TraceContextArg<Clean>) {
		return "TraceContext";
	} else if constexpr (BearerArg<Clean>) {
		return "BearerToken";
	} else if constexpr (RequiredBearerArg<Clean>) {
		return "RequiredBearerToken";
	} else if constexpr (OptionalBearerArg<Clean>) {
		return "OptionalBearerToken";
	} else if constexpr (BasicAuthArg<Clean>) {
		return "BasicAuth";
	} else if constexpr (RequiredBasicAuthArg<Clean>) {
		return "RequiredBasicAuth";
	} else if constexpr (OptionalBasicAuthArg<Clean>) {
		return "OptionalBasicAuth";
	} else if constexpr (JsonArg<Clean>) {
		return "Json";
	} else if constexpr (RequestViewArg<Clean>) {
		return "RequestView";
	} else if constexpr (RequestArg<Clean>) {
		return "Request";
	} else {
		return "unknown";
	}
}

template<class Args, std::size_t... Is>
void append_extractors(
	std::vector<std::string> &out,
	std::index_sequence<Is...>) {
	(out.push_back(extractor_name<std::tuple_element_t<Is, Args>>()), ...);
}

template<class Args, std::size_t... Is>
void append_path_extractors(
	std::vector<std::string> &out,
	std::index_sequence<Is...>) {
	(
		[&] {
			using Arg = std::tuple_element_t<Is, Args>;
			using Clean = std::remove_cvref_t<Arg>;
			if constexpr (PathArg<Clean>) {
				out.push_back(std::string{PathType<Clean>::name.view()});
			}
		}(),
		...);
}

template<class T>
[[nodiscard]] consteval std::string_view route_type_tag() {
	using Clean = std::remove_cvref_t<T>;
	if constexpr (std::same_as<Clean, std::uint64_t>) {
		return "u64";
	} else if constexpr (std::same_as<Clean, std::int64_t>) {
		return "i64";
	} else if constexpr (std::same_as<Clean, std::uint32_t>) {
		return "u32";
	} else if constexpr (std::same_as<Clean, std::int32_t>) {
		return "i32";
	} else if constexpr (std::same_as<Clean, std::string> || std::same_as<Clean, std::string_view>) {
		return "string";
	} else {
		return "";
	}
}

template<class Args, std::size_t... Is>
void append_path_extractor_types(
	std::vector<std::pair<std::string, std::string>> &out,
	std::vector<std::pair<std::size_t, std::string>> &index_out,
	std::index_sequence<Is...>) {
	(
		[&] {
			using Arg = std::tuple_element_t<Is, Args>;
			using Clean = std::remove_cvref_t<Arg>;
			if constexpr (PathArg<Clean>) {
				using PathValue = typename PathType<Clean>::type;
				out.emplace_back(std::string{PathType<Clean>::name.view()}, std::string{route_type_tag<PathValue>()});
			} else if constexpr (PathAtArg<Clean>) {
				using PathValue = typename PathAtType<Clean>::type;
				index_out.emplace_back(PathAtType<Clean>::index, std::string{route_type_tag<PathValue>()});
			}
		}(),
		...);
}

template<class Args, std::size_t... Is>
[[nodiscard]] consteval bool has_body_extractor_impl(
	std::index_sequence<Is...>) {
	return (
		false
		|| ...
		|| (BodyTextArg<std::tuple_element_t<Is, Args>>
			|| BodyBytesArg<std::tuple_element_t<Is, Args>>
			|| OwnedBodyBytesArg<std::tuple_element_t<Is, Args>>
#if CONFLUX_HAS_JSON
			|| FormParamsArg<std::tuple_element_t<Is, Args>>
			|| JsonDocumentArg<std::tuple_element_t<Is, Args>>
			|| JsonPatchArg<std::tuple_element_t<Is, Args>>
			|| MergePatchArg<std::tuple_element_t<Is, Args>>
#endif
			|| MultipartArg<std::tuple_element_t<Is, Args>>
			|| JsonArg<std::tuple_element_t<Is, Args>>));
}

template<class Args>
[[nodiscard]] consteval bool has_body_extractor() {
	return has_body_extractor_impl<Args>(std::make_index_sequence<std::tuple_size_v<Args>>{});
}

} // namespace conflux::http::detail
