export module conflux.json.reflect_provider;

import std;
import conflux.types;
import conflux.json;
export import conflux.json.boundary;
export import conflux.json.native_provider;
export import conflux.json.reflect;

namespace conflux::json::boundary::detail {

[[nodiscard]] inline ErrorStage map_reflect_stage(
	JsonStage stage) noexcept {
	switch (stage) {
	case JsonStage::parse     : return ErrorStage::parse;
	case JsonStage::lookup    : return ErrorStage::lookup;
	case JsonStage::decode    : return ErrorStage::decode;
	case JsonStage::build     : return ErrorStage::build;
	case JsonStage::dump      : return ErrorStage::dump;
	case JsonStage::json_patch: return ErrorStage::json_patch;
	}
	return ErrorStage::provider;
}

[[nodiscard]] inline ErrorCode map_reflect_code(
	JsonIssueCode code) noexcept {
	switch (code) {
	case JsonIssueCode::syntax_error                  : return ErrorCode::syntax_error;
	case JsonIssueCode::unexpected_eof                : return ErrorCode::unexpected_eof;
	case JsonIssueCode::trailing_garbage              : return ErrorCode::trailing_garbage;
	case JsonIssueCode::input_too_large               : return ErrorCode::input_too_large;
	case JsonIssueCode::string_too_large              : return ErrorCode::string_too_large;
	case JsonIssueCode::nesting_too_deep              : return ErrorCode::nesting_too_deep;
	case JsonIssueCode::wrong_kind                    : return ErrorCode::wrong_kind;
	case JsonIssueCode::missing_member                : return ErrorCode::missing_member;
	case JsonIssueCode::index_out_of_range            : return ErrorCode::index_out_of_range;
	case JsonIssueCode::invalid_number                : return ErrorCode::invalid_number;
	case JsonIssueCode::number_out_of_range           : return ErrorCode::number_out_of_range;
	case JsonIssueCode::sign_mismatch                 : return ErrorCode::sign_mismatch;
	case JsonIssueCode::duplicate_member              : return ErrorCode::duplicate_member;
	case JsonIssueCode::invalid_unicode_escape        : return ErrorCode::invalid_unicode_escape;
	case JsonIssueCode::invalid_utf8                  : return ErrorCode::invalid_utf8;
	case JsonIssueCode::invalid_pointer               : return ErrorCode::invalid_pointer;
	case JsonIssueCode::constraint_violation          : return ErrorCode::constraint_violation;
	case JsonIssueCode::invalid_value                 : return ErrorCode::invalid_value;
	case JsonIssueCode::output_too_large              : return ErrorCode::output_too_large;
	case JsonIssueCode::resource_exhausted            : return ErrorCode::resource_exhausted;
	case JsonIssueCode::invalid_patch                 :
	case JsonIssueCode::patch_op_missing              :
	case JsonIssueCode::patch_op_unknown              :
	case JsonIssueCode::patch_path_missing            :
	case JsonIssueCode::patch_path_invalid            :
	case JsonIssueCode::patch_from_missing            :
	case JsonIssueCode::patch_from_invalid            :
	case JsonIssueCode::patch_test_failed             :
	case JsonIssueCode::patch_target_missing          :
	case JsonIssueCode::patch_parent_missing          :
	case JsonIssueCode::patch_array_index_invalid     :
	case JsonIssueCode::patch_array_index_out_of_range:
	case JsonIssueCode::patch_move_into_child         :
	case JsonIssueCode::patch_remove_document_root    :
	case JsonIssueCode::patch_too_many_operations     :
	case JsonIssueCode::patch_pointer_too_deep        : return ErrorCode::invalid_patch;
	}
	return ErrorCode::provider_failure;
}

[[nodiscard]] inline SourceLocation map_reflect_source(
	JsonSourceLocation const &source) noexcept {
	return SourceLocation{.offset = source.offset, .line = source.line, .column = source.column};
}

[[nodiscard]] inline JsonDumpOptions map_reflect_dump_options(
	DumpOptions const &opts) noexcept {
	return JsonDumpOptions{
		.pretty = opts.pretty,
		.indent = opts.indent,
		.sort_object_keys = opts.sort_object_keys,
		.ascii_only = opts.ascii_only,
	};
}

[[nodiscard]] inline Error map_reflect_error(
	JsonError const &err) {
	Error out{
		.stage = map_reflect_stage(err.stage),
		.code = map_reflect_code(err.code),
		.message = err.message,
	};
	if (err.source) {
		out.source = map_reflect_source(*err.source);
	}
	if (err.member_name) {
		out.member_name = *err.member_name;
	}
	return out;
}

} // namespace conflux::json::boundary::detail

export namespace conflux::json::boundary {

// Native JSON provider edge that imports the P2996 reflection codec. Framework
// and HTTP helpers still bind through the provider-neutral boundary concepts;
// this module only supplies the native reflected provider type for callers that
// want zero-boilerplate aggregate serde.
struct NativeReflectJsonProvider {
	using document_type = NativeJsonProvider::document_type;

	[[nodiscard]] static std::expected<document_type, Error> parse_json_document(
		std::string_view input,
		DecodeOptions const &opts = {}) {
		return NativeJsonProvider::parse_json_document(input, opts);
	}

	[[nodiscard]] static std::expected<std::string, Error> dump_json(
		document_type const &doc,
		DumpOptions const &opts = {}) {
		return NativeJsonProvider::dump_json(doc, opts);
	}

	template<class T>
		requires JsonDumpProvider<NativeJsonProvider, std::remove_cvref_t<T>>
	[[nodiscard]] static std::expected<std::string, Error> dump_json(
		T const &value,
		DumpOptions const &opts = {}) {
		if constexpr (ReflectJsonAggregate<std::remove_cvref_t<T>>) {
			auto body = dump_reflect_direct<std::remove_cvref_t<T>>(value, detail::map_reflect_dump_options(opts));
			if (body) {
				return std::move(*body);
			}
			if (!opts.sort_object_keys) {
				return std::unexpected(detail::map_reflect_error(body.error()));
			}
		}
		return NativeJsonProvider::dump_json(value, opts);
	}

	template<class T>
		requires JsonDecodeProvider<NativeJsonProvider, std::remove_cvref_t<T>>
	[[nodiscard]] static std::expected<std::remove_cvref_t<T>, Error> decode_json(
		std::string_view input,
		DecodeOptions const &opts = {}) {
		return NativeJsonProvider::template decode_json<std::remove_cvref_t<T>>(input, opts);
	}
};

template<class T>
using NativeReflectSerdeTraits = SerdeTraits<NativeReflectJsonProvider, T>;

template<class T>
concept NativeReflectJsonSerializable = JsonDumpProvider<NativeReflectJsonProvider, T>;

template<class T>
concept NativeReflectJsonDecodable = JsonDecodeProvider<NativeReflectJsonProvider, T>;

template<class T>
[[nodiscard]] std::expected<std::string, Error> dump_native_reflect(
	T const &value,
	DumpOptions const &opts = {})
	requires NativeReflectJsonSerializable<std::remove_cvref_t<T>>
{
	return dump_with<NativeReflectJsonProvider>(value, opts);
}

template<class T>
[[nodiscard]] std::expected<std::remove_cvref_t<T>, Error> decode_native_reflect(
	std::string_view input,
	DecodeOptions const &opts = {})
	requires NativeReflectJsonDecodable<std::remove_cvref_t<T>>
{
	return decode_with<NativeReflectJsonProvider, std::remove_cvref_t<T>>(input, opts);
}

} // namespace conflux::json::boundary
