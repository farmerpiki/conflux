module;

export module conflux.net.app.json_helpers;

import std;
import conflux.net.http.types;
import conflux.net.http.parse_helpers;
import conflux.net.http.response;
import conflux.net.http.json_string;
import conflux.utils;
#if CONFLUX_HAS_JSON
import conflux.json;
import conflux.net.http.native_json;
#endif

export namespace conflux::http::detail {

#if CONFLUX_HAS_JSON
[[nodiscard]] std::string_view content_type_media_type(
	std::string_view content_type) noexcept {
	return conflux::http::content_type_media_type(content_type);
}

[[nodiscard]] bool content_type_matches(
	std::string_view content_type,
	std::string_view expected) noexcept {
	return conflux::http::content_type_matches(content_type, expected);
}

[[nodiscard]] bool content_type_is_json(
	std::string_view content_type) noexcept {
	return content_type_matches(content_type, "application/json");
}

[[nodiscard]] bool content_type_is_problem_json(
	std::string_view content_type) noexcept {
	return content_type_matches(content_type, "application/problem+json");
}

[[nodiscard]] bool content_type_is_json_request(
	std::string_view content_type) noexcept {
	return content_type_is_json(content_type) || content_type_is_problem_json(content_type);
}

[[nodiscard]] bool content_type_is_json_patch(
	std::string_view content_type) noexcept {
	return content_type_matches(content_type, "application/json-patch+json");
}

[[nodiscard]] bool content_type_is_merge_patch(
	std::string_view content_type) noexcept {
	return content_type_matches(content_type, "application/merge-patch+json");
}

template<class T>
[[nodiscard]] std::string schema_json_or_object() {
	if constexpr (requires { conflux::json::schema_for<std::remove_cvref_t<T>>(); }) {
		auto schema = conflux::json::schema_for<std::remove_cvref_t<T>>();
		if (!schema) {
			return R"({"type":"object"})";
		}
		auto dumped = schema->dump();
		if (!dumped) {
			return R"({"type":"object"})";
		}
		return std::move(*dumped);
	} else {
		return R"({"type":"object"})";
	}
}

[[nodiscard]] std::string_view json_error_stage_name(
	conflux::json::boundary::ErrorStage stage) noexcept {
	using enum conflux::json::boundary::ErrorStage;
	switch (stage) {
	case parse     : return "parse";
	case lookup    : return "lookup";
	case decode    : return "decode";
	case build     : return "build";
	case dump      : return "dump";
	case json_patch: return "json_patch";
	case provider  : return "provider";
	}
	return "provider";
}

[[nodiscard]] std::string_view json_error_code_name(
	conflux::json::boundary::ErrorCode code) noexcept {
	using enum conflux::json::boundary::ErrorCode;
	switch (code) {
	case provider_failure      : return "provider_failure";
	case syntax_error          : return "syntax_error";
	case unexpected_eof        : return "unexpected_eof";
	case trailing_garbage      : return "trailing_garbage";
	case input_too_large       : return "input_too_large";
	case string_too_large      : return "string_too_large";
	case nesting_too_deep      : return "nesting_too_deep";
	case wrong_kind            : return "wrong_kind";
	case missing_member        : return "missing_member";
	case index_out_of_range    : return "index_out_of_range";
	case invalid_number        : return "invalid_number";
	case number_out_of_range   : return "number_out_of_range";
	case sign_mismatch         : return "sign_mismatch";
	case duplicate_member      : return "duplicate_member";
	case invalid_unicode_escape: return "invalid_unicode_escape";
	case invalid_utf8          : return "invalid_utf8";
	case invalid_pointer       : return "invalid_pointer";
	case constraint_violation  : return "constraint_violation";
	case invalid_value         : return "invalid_value";
	case output_too_large      : return "output_too_large";
	case resource_exhausted    : return "resource_exhausted";
	case invalid_patch         : return "invalid_patch";
	}
	return "provider_failure";
}

[[nodiscard]] std::string_view json_issue_code_name(
	json::JsonIssueCode code) noexcept {
	using enum json::JsonIssueCode;
	switch (code) {
	case invalid_patch                 : return "invalid_patch";
	case patch_op_missing              : return "patch_op_missing";
	case patch_op_unknown              : return "patch_op_unknown";
	case patch_path_missing            : return "patch_path_missing";
	case patch_path_invalid            : return "patch_path_invalid";
	case patch_from_missing            : return "patch_from_missing";
	case patch_from_invalid            : return "patch_from_invalid";
	case patch_test_failed             : return "patch_test_failed";
	case patch_target_missing          : return "patch_target_missing";
	case patch_parent_missing          : return "patch_parent_missing";
	case patch_array_index_invalid     : return "patch_array_index_invalid";
	case patch_array_index_out_of_range: return "patch_array_index_out_of_range";
	case patch_move_into_child         : return "patch_move_into_child";
	case patch_remove_document_root    : return "patch_remove_document_root";
	case patch_too_many_operations     : return "patch_too_many_operations";
	case patch_pointer_too_deep        : return "patch_pointer_too_deep";
	default                            : return "invalid_json";
	}
}

[[nodiscard]] Response json_decode_problem(
	conflux::json::boundary::Error const &err) {
	std::string body = std::format(
		R"({{"code":"json.decode.type_mismatch","stage":{},"kind":{},"detail":{})",
		json_string(json_error_stage_name(err.stage)),
		json_string(json_error_code_name(err.code)),
		json_string(err.message));
	if (err.member_name) {
		body += std::format(R"(,"member":{})", json_string(*err.member_name));
	}
	if (err.source) {
		body += std::format(
			R"(,"source":{{"offset":{},"line":{},"column":{}}})",
			err.source->offset,
			err.source->line,
			err.source->column);
	}
	body += "}";
	return Response::problem_json(std::move(body), kHttpBadRequest, "Bad Request");
}

[[nodiscard]] Response json_patch_problem(
	json::JsonError const &err) {
	std::string body = std::format(
		R"({{"code":{},"stage":"json_patch","detail":{})",
		json_string(json_issue_code_name(err.code)),
		json_string(err.message));
	if (err.operation_index) {
		body += std::format(R"(,"operation_index":{})", *err.operation_index);
	}
	if (err.operation) {
		body += std::format(R"(,"operation":{})", json_string(*err.operation));
	}
	if (err.pointer) {
		body += std::format(R"(,"path":{})", json_string(*err.pointer));
	}
	if (err.from_pointer) {
		body += std::format(R"(,"from":{})", json_string(*err.from_pointer));
	}
	body += "}";
	return Response::problem_json(std::move(body), kHttpBadRequest, "Bad Request");
}

[[nodiscard]] Response unsupported_json_content_type_problem() {
	return Response::problem_json(
		R"({"code":"unsupported_content_type","detail":"expected application/json","expected":"application/json"})",
		kHttpBadRequest,
		"Bad Request");
}

[[nodiscard]] Response json_body_too_large_problem() {
	return Response::problem_json(
		R"({"code":"body_too_large","detail":"request body exceeds the configured limit"})",
		kHttpRequestEntityTooLarge,
		"Content Too Large");
}
#endif

} // namespace conflux::http::detail
