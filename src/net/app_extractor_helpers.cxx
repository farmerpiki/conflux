export module conflux.net.app.extractor_helpers;

import std;
import conflux.net.app.types;
import conflux.net.http.types;
import conflux.net.http.response;
import conflux.net.http.server_types;
import conflux.utils;
#if CONFLUX_HAS_JSON
import conflux.json;
#endif

export namespace conflux::http::detail {

[[nodiscard]] std::string field_problem_json_string(
	std::string_view value) {
#if CONFLUX_HAS_JSON
	auto dumped = dump_direct(value);
	if (dumped) {
		return std::move(*dumped);
	}
#endif
	return json_string_fallback(value);
}

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
		R"({{"code":"invalid_field","extractor":{},"source":{},"name":{},"kind":{},"detail":{}}})",
		field_problem_json_string(extractor),
		field_problem_json_string(http_field_source_name(err.source)),
		field_problem_json_string(err.name),
		field_problem_json_string(kind),
		field_problem_json_string(err.message));
	return Response::problem_json(std::move(body), kHttpBadRequest, "Bad Request");
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

[[nodiscard]] std::optional<std::pair<std::string_view, std::string_view>> path_param_at(
	RequestView const &req,
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
	RequestView const &req,
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
	RequestView const &req,
	Members const &members,
	std::index_sequence<Is...>) {
	T out{};
	(
		[&] {
			auto const &member = std::get<Is>(members);
			using MemberValue = std::remove_cvref_t<decltype(out.*(member.pointer))>;
			out.*(member.pointer) = extract_or_throw(req.template query_as<MemberValue>(member.name), "QueryParams");
		}(),
		...);
	return out;
}

template<class T>
[[nodiscard]] T extract_query_params(
	RequestView const &req) {
	auto const members = JsonMembers<T>::members();
	return extract_query_params_impl<T>(
		req,
		members,
		std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<decltype(members)>>>{});
}

template<class T, class Members, std::size_t... Is>
[[nodiscard]] T extract_form_params_impl(
	RequestView const &req,
	Members const &members,
	std::index_sequence<Is...>) {
	T out{};
	(
		[&] {
			auto const &member = std::get<Is>(members);
			using MemberValue = std::remove_cvref_t<decltype(out.*(member.pointer))>;
			out.*(member.pointer) = extract_or_throw(req.template form_as<MemberValue>(member.name), "FormParams");
		}(),
		...);
	return out;
}

template<class T>
[[nodiscard]] T extract_form_params(
	RequestView const &req) {
	auto const members = JsonMembers<T>::members();
	return extract_form_params_impl<T>(
		req,
		members,
		std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<decltype(members)>>>{});
}
#endif

} // namespace conflux::http::detail
