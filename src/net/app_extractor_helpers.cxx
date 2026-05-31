export module conflux.net.app.extractor_helpers;

import std;
import conflux.net.app.types;
import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.http.server_types;
import conflux.net.http.json_string;
import conflux.utils;
#if CONFLUX_HAS_JSON
import conflux.json;
#endif

export namespace conflux::http::detail {

template<class Arg>
[[nodiscard]] auto field_problem(
	std::string_view extractor,
	HttpFieldError const &err) {
	auto kind = [err] {
		switch (err.kind) {
		case HttpFieldErrorKind::missing     : return "missing";
		case HttpFieldErrorKind::empty       : return "empty";
		case HttpFieldErrorKind::invalid     : return "invalid";
		case HttpFieldErrorKind::out_of_range: return "out_of_range";
		}
		return "invalid";
	}();
	auto body = std::format(
		R"({{"code":"invalid_field","extractor":{},"source":{},"name":{},"kind":{},"detail":{})",
		json_string(extractor),
		json_string(http_field_source_name(err.source)),
		json_string(err.name),
		json_string(kind),
		json_string(err.message));
#ifndef NDEBUG
	body += R"(,"target":)";
	body += json_string(typeid(Arg).name());
#endif
	body += '}';
	return Response::problem_json(std::move(body), kHttpBadRequest, "Bad conflux::http::OwnedRequest");
}

template<class T>
[[nodiscard]] T extract_or_throw(
	std::expected<T, HttpFieldError> value,
	std::string_view extractor) {
	if (!value) {
		throw ExtractorFailure{field_problem<T>(extractor, value.error())};
	}
	return std::move(*value);
}

template<class T>
struct OptionalFieldType {};

template<class T>
struct OptionalFieldType<std::optional<T>> {
	using type = T;
};

template<class T>
concept OptionalFieldValue = requires { typename OptionalFieldType<std::remove_cvref_t<T>>::type; };

[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>> path_param_at(
	conflux::http::RequestView const &req,
	std::size_t index) noexcept {
	std::size_t i = 0;
	for (auto const &[name, value]: req.params) {
		if (i == index) {
			return std::pair<std::string_view, std::string_view>{name, value};
		}
		++i;
	}
	return std::nullopt;
}

template<class T>
[[nodiscard]] std::expected<T, HttpFieldError> path_param_as_at(
	conflux::http::RequestView const &req,
	std::size_t index) {
	auto param = path_param_at(req, index);
	auto name = std::format("#{}", index);
	if (!param) {
		auto err = HttpFieldError{
			.kind = HttpFieldErrorKind::missing,
			.source = HttpFieldSource::params,
			.name = std::move(name),
			.message = std::format("params field '#{}' is missing", index)};
		return std::unexpected{std::move(err)};
	}
	return parse_http_field_value<T>(param->second, HttpFieldSource::params, param->first);
}

#if CONFLUX_HAS_JSON
template<class T, class Members, std::size_t... Is>
[[nodiscard]] T extract_query_params_impl(
	conflux::http::RequestView const &req,
	Members const &members,
	std::index_sequence<Is...>) {
	T out{};
	(
		[&] {
			auto const &member = std::get<Is>(members);
			using MemberValue = std::remove_cvref_t<decltype(out.*(member.pointer))>;
			if constexpr (OptionalFieldValue<MemberValue>) {
				using FieldValue = typename OptionalFieldType<MemberValue>::type;
				out.*(member.pointer) =
					extract_or_throw(req.template optional_query_as<FieldValue>(member.name), "QueryParams");
			} else {
				out.*(member.pointer) =
					extract_or_throw(req.template query_as<MemberValue>(member.name), "QueryParams");
			}
		}(),
		...);
	return out;
}

template<class T>
[[nodiscard]] T extract_query_params(
	conflux::http::RequestView const &req) {
	auto const members = conflux::json::JsonMembers<T>::members();
	return extract_query_params_impl<T>(
		req,
		members,
		std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<decltype(members)>>>{});
}

template<class T, class Members, std::size_t... Is>
[[nodiscard]] T extract_form_params_impl(
	conflux::http::RequestView const &req,
	Members const &members,
	std::index_sequence<Is...>) {
	T out{};
	(
		[&] {
			auto const &member = std::get<Is>(members);
			using MemberValue = std::remove_cvref_t<decltype(out.*(member.pointer))>;
			if constexpr (OptionalFieldValue<MemberValue>) {
				using FieldValue = typename OptionalFieldType<MemberValue>::type;
				out.*(member.pointer) =
					extract_or_throw(req.template optional_form_as<FieldValue>(member.name), "FormParams");
			} else {
				out.*(member.pointer) = extract_or_throw(req.template form_as<MemberValue>(member.name), "FormParams");
			}
		}(),
		...);
	return out;
}

template<class T>
[[nodiscard]] T extract_form_params(
	conflux::http::RequestView const &req) {
	auto const members = conflux::json::JsonMembers<T>::members();
	return extract_form_params_impl<T>(
		req,
		members,
		std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<decltype(members)>>>{});
}
#endif

} // namespace conflux::http::detail
