export module conflux.json.native_provider;

import std;
import conflux.types;
import conflux.json;
export import conflux.json.boundary;

namespace conflux::json::boundary::detail {

[[nodiscard]] inline ErrorStage map_stage(
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

[[nodiscard]] inline ErrorCode map_code(
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

[[nodiscard]] inline decltype(JsonDecodeOptions{}.unknown_members) map_unknown_member_policy(
	UnknownMemberPolicy policy) noexcept {
	using DecodePolicy = decltype(JsonDecodeOptions{}.unknown_members);
	switch (policy) {
	case UnknownMemberPolicy::reject: return DecodePolicy::reject;
	case UnknownMemberPolicy::ignore: return DecodePolicy::ignore;
	}
	return DecodePolicy::reject;
}

[[nodiscard]] inline JsonDecodeOptions map_decode_options(
	DecodeOptions const &opts) noexcept {
	return JsonDecodeOptions{.unknown_members = map_unknown_member_policy(opts.unknown_members)};
}

[[nodiscard]] inline JsonDomPolicy map_dom_policy(
	DecodeOptions const &opts) noexcept {
	return opts.copy_input ? JsonDomPolicy::owning_document() : JsonDomPolicy::view_first();
}

} // namespace conflux::json::boundary::detail

export namespace conflux::json::boundary {

struct NativeJsonProvider {
	using document_type = Document;

	[[nodiscard]] static std::expected<Document, Error> parse_json_document(
		std::string_view input,
		DecodeOptions const &opts = {}) {
		auto doc = parse_dom(input, detail::map_dom_policy(opts));
		if (!doc) {
			return std::unexpected(detail::map_error(doc.error()));
		}
		return std::move(*doc);
	}

	[[nodiscard]] static std::expected<std::string, Error> dump_json(
		Document const &doc,
		DumpOptions const &opts = {}) {
		auto body = doc.dump(detail::map_dump_options(opts));
		if (!body) {
			return std::unexpected(detail::map_error(body.error()));
		}
		return std::move(*body);
	}

	template<class T>
		requires has_json_codec<std::remove_cvref_t<T>>
	[[nodiscard]] static std::expected<std::string, Error> dump_json(
		T const &value,
		DumpOptions const &opts = {}) {
		if constexpr (JsonDirectWritable<std::remove_cvref_t<T>>) {
			auto body = dump_direct<std::remove_cvref_t<T>>(value, detail::map_dump_options(opts));
			if (body) {
				return std::move(*body);
			}
			if (!opts.sort_object_keys) {
				return std::unexpected(detail::map_error(body.error()));
			}
		}
		ValueBuilder builder;
		if (auto ok = builder.template set<std::remove_cvref_t<T>>(value); !ok) {
			return std::unexpected(detail::map_error(ok.error()));
		}
		auto doc = std::move(builder).finish();
		if (!doc) {
			return std::unexpected(detail::map_error(doc.error()));
		}
		return dump_json(*doc, opts);
	}

	template<class T, class Sink>
		requires has_json_codec<std::remove_cvref_t<T>>
	[[nodiscard]] static std::expected<void, Error> write_json(
		T const &value,
		DumpOptions const &opts,
		Sink &&sink) {
		if constexpr (JsonDirectWritable<std::remove_cvref_t<T>>) {
			auto ok = write_json_direct_to<std::remove_cvref_t<T>>(value, detail::map_dump_options(opts), sink);
			if (ok) {
				return {};
			}
			if (!opts.sort_object_keys) {
				return std::unexpected(detail::map_error(ok.error()));
			}
		}
		auto dumped = dump_json(value, opts);
		if (!dumped) {
			return std::unexpected(dumped.error());
		}
		std::invoke(std::forward<Sink>(sink), std::string_view{*dumped});
		return {};
	}

	template<class T>
		requires has_json_codec<std::remove_cvref_t<T>>
	[[nodiscard]] static std::expected<std::remove_cvref_t<T>, Error> decode_json(
		std::string_view input,
		DecodeOptions const &opts = {}) {
		using Raw = std::remove_cvref_t<T>;
		if constexpr (json_contains_borrowed_view_v<Raw>) {
			return std::unexpected(
				Error{
					.stage = ErrorStage::decode,
					.code = ErrorCode::invalid_value,
					.message = opts.copy_input ? "copy_input=true cannot safely decode borrowed-view fields" :
												 "borrowed-view direct decode is not supported by NativeJsonProvider"});
		} else {
			auto const decode_opts = detail::map_decode_options(opts);
			if (opts.copy_input) {
				auto doc = parse_dom(input, detail::map_dom_policy(opts));
				if (!doc) {
					return std::unexpected(detail::map_error(doc.error()));
				}
				auto decoded = decode<Raw>(*doc, decode_opts);
				if (!decoded) {
					return std::unexpected(detail::map_error(decoded.error()));
				}
				return std::move(*decoded);
			}
			Raw out{};
			auto decoded = decode_full_into<Raw>(out, input, detail::map_dom_policy(opts).parse, decode_opts);
			if (!decoded) {
				return std::unexpected(detail::map_error(decoded.error()));
			}
			return out;
		}
	}
};

template<class T>
using NativeSerdeTraits = SerdeTraits<NativeJsonProvider, T>;

template<class T>
concept NativeJsonSerializable = JsonDumpProvider<NativeJsonProvider, T>;

template<class T>
concept NativeJsonDecodable = JsonDecodeProvider<NativeJsonProvider, T>;

template<class T>
[[nodiscard]] std::expected<std::string, Error> dump_native(
	T const &value,
	DumpOptions const &opts = {})
	requires NativeJsonSerializable<T>
{
	return NativeJsonProvider::dump_json(value, opts);
}

template<class T>
[[nodiscard]] std::expected<std::remove_cvref_t<T>, Error> decode_native(
	std::string_view input,
	DecodeOptions const &opts = {})
	requires NativeJsonDecodable<std::remove_cvref_t<T>>
{
	return NativeJsonProvider::template decode_json<std::remove_cvref_t<T>>(input, opts);
}

} // namespace conflux::json::boundary
