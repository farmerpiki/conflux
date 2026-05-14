export module conflux.json.native_provider;

import std;
import conflux.types;
import conflux.json;
export import conflux.json.boundary;

namespace conflux::json::boundary::detail {

[[nodiscard]] inline ErrorStage map_stage(
	JsonStage stage) noexcept {
	switch (stage) {
	case JsonStage::parse : return ErrorStage::parse;
	case JsonStage::lookup: return ErrorStage::lookup;
	case JsonStage::decode: return ErrorStage::decode;
	case JsonStage::build : return ErrorStage::build;
	case JsonStage::dump  : return ErrorStage::dump;
	}
	return ErrorStage::provider;
}

[[nodiscard]] inline ErrorCode map_code(
	JsonIssueCode code) noexcept {
	switch (code) {
	case JsonIssueCode::syntax_error          : return ErrorCode::syntax_error;
	case JsonIssueCode::unexpected_eof        : return ErrorCode::unexpected_eof;
	case JsonIssueCode::trailing_garbage      : return ErrorCode::trailing_garbage;
	case JsonIssueCode::input_too_large       : return ErrorCode::input_too_large;
	case JsonIssueCode::string_too_large      : return ErrorCode::string_too_large;
	case JsonIssueCode::nesting_too_deep      : return ErrorCode::nesting_too_deep;
	case JsonIssueCode::wrong_kind            : return ErrorCode::wrong_kind;
	case JsonIssueCode::missing_member        : return ErrorCode::missing_member;
	case JsonIssueCode::index_out_of_range    : return ErrorCode::index_out_of_range;
	case JsonIssueCode::invalid_number        : return ErrorCode::invalid_number;
	case JsonIssueCode::number_out_of_range   : return ErrorCode::number_out_of_range;
	case JsonIssueCode::sign_mismatch         : return ErrorCode::sign_mismatch;
	case JsonIssueCode::duplicate_member      : return ErrorCode::duplicate_member;
	case JsonIssueCode::invalid_unicode_escape: return ErrorCode::invalid_unicode_escape;
	case JsonIssueCode::invalid_utf8          : return ErrorCode::invalid_utf8;
	case JsonIssueCode::invalid_pointer       : return ErrorCode::invalid_pointer;
	case JsonIssueCode::constraint_violation  : return ErrorCode::constraint_violation;
	case JsonIssueCode::invalid_value         : return ErrorCode::invalid_value;
	case JsonIssueCode::output_too_large      : return ErrorCode::output_too_large;
	case JsonIssueCode::resource_exhausted    : return ErrorCode::resource_exhausted;
	}
	return ErrorCode::provider_failure;
}

[[nodiscard]] inline SourceLocation map_source(
	JsonSourceLocation const &source) noexcept {
	return SourceLocation{.offset = source.offset, .line = source.line, .column = source.column};
}

[[nodiscard]] inline Error map_error(
	JsonError const &err) {
	Error out{
		.stage = map_stage(err.stage),
		.code = map_code(err.code),
		.message = err.message,
	};
	if (err.source) {
		out.source = map_source(*err.source);
	}
	if (err.member_name) {
		out.member_name = *err.member_name;
	}
	return out;
}

[[nodiscard]] inline JsonDumpOptions map_dump_options(
	DumpOptions const &opts) noexcept {
	return JsonDumpOptions{
		.pretty = opts.pretty,
		.indent = opts.indent,
		.sort_object_keys = opts.sort_object_keys,
		.ascii_only = opts.ascii_only,
	};
}

} // namespace conflux::json::boundary::detail

export namespace conflux::json::boundary {

struct NativeJsonProvider {
	using document_type = Document;

	[[nodiscard]] static expected<Document, Error> parse_json_document(
		SV input,
		DecodeOptions const &opts = {}) {
		auto doc = opts.copy_input ? parse_copy(input) : parse_view(input);
		if (!doc) {
			return unexpected(detail::map_error(doc.error()));
		}
		return move(*doc);
	}

	[[nodiscard]] static expected<S, Error> dump_json(
		Document const &doc,
		DumpOptions const &opts = {}) {
		auto body = doc.dump(detail::map_dump_options(opts));
		if (!body) {
			return unexpected(detail::map_error(body.error()));
		}
		return move(*body);
	}

	template<class T>
		requires has_json_codec<std::remove_cvref_t<T>>
	[[nodiscard]] static expected<S, Error> dump_json(
		T const &value,
		DumpOptions const &opts = {}) {
		ValueBuilder builder;
		if (auto ok = builder.template set<std::remove_cvref_t<T>>(value); !ok) {
			return unexpected(detail::map_error(ok.error()));
		}
		auto doc = move(builder).finish();
		if (!doc) {
			return unexpected(detail::map_error(doc.error()));
		}
		return dump_json(*doc, opts);
	}

	template<class T>
		requires has_json_codec<std::remove_cvref_t<T>>
	[[nodiscard]] static expected<std::remove_cvref_t<T>, Error> decode_json(
		SV input,
		DecodeOptions const &opts = {}) {
		if (opts.copy_input) {
			auto doc = parse_copy(input);
			if (!doc) {
				return unexpected(detail::map_error(doc.error()));
			}
			auto decoded = decode<std::remove_cvref_t<T>>(*doc);
			if (!decoded) {
				return unexpected(detail::map_error(decoded.error()));
			}
			return move(*decoded);
		}
		JsonReader reader{input};
		auto decoded = decode_full<std::remove_cvref_t<T>>(reader);
		if (!decoded) {
			return unexpected(detail::map_error(decoded.error()));
		}
		return move(*decoded);
	}
};

template<class T>
using NativeSerdeTraits = SerdeTraits<NativeJsonProvider, T>;

template<class T>
concept NativeJsonSerializable = JsonDumpProvider<NativeJsonProvider, T>;

template<class T>
concept NativeJsonDecodable = JsonDecodeProvider<NativeJsonProvider, T>;

template<class T>
[[nodiscard]] expected<S, Error> dump_native(
	T const &value,
	DumpOptions const &opts = {})
	requires NativeJsonSerializable<T>
{
	return dump_with<NativeJsonProvider>(value, opts);
}

template<class T>
[[nodiscard]] expected<std::remove_cvref_t<T>, Error> decode_native(
	SV input,
	DecodeOptions const &opts = {})
	requires NativeJsonDecodable<std::remove_cvref_t<T>>
{
	return decode_with<NativeJsonProvider, std::remove_cvref_t<T>>(input, opts);
}

} // namespace conflux::json::boundary
