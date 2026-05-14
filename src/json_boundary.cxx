export module conflux.json.boundary;

import std;
import conflux.types;

export namespace conflux::json::boundary {

inline constexpr SV kContentType = "application/json";

// Provider-independent dump options. Native conflux JSON maps these directly to
// JsonDumpOptions; alternate providers can ignore fields they do not support.
struct DumpOptions {
	bool pretty{false};
	unsigned indent{2};
	bool sort_object_keys{false};
	bool ascii_only{false};
};

// Provider-independent parse/decode boundary options. The native provider uses
// parse_copy when copy_input is true and parse_view/JsonReader otherwise.
struct DecodeOptions {
	bool copy_input{true};
};

// Keep the boundary error small and provider-neutral. Provider-specific error
// details may be summarized in message; direct provider errors stay behind the
// provider module.
enum class ErrorStage : u8 {
	parse,
	lookup,
	decode,
	build,
	dump,
	provider,
};

enum class ErrorCode : u8 {
	provider_failure,
	syntax_error,
	unexpected_eof,
	trailing_garbage,
	input_too_large,
	string_too_large,
	nesting_too_deep,
	wrong_kind,
	missing_member,
	index_out_of_range,
	invalid_number,
	number_out_of_range,
	sign_mismatch,
	duplicate_member,
	invalid_unicode_escape,
	invalid_utf8,
	invalid_pointer,
	constraint_violation,
	invalid_value,
	output_too_large,
	resource_exhausted,
};

struct SourceLocation {
	SZ offset{};
	SZ line{1};
	SZ column{1};
};

struct Error {
	ErrorStage stage{ErrorStage::provider};
	ErrorCode code{ErrorCode::provider_failure};
	S message{};
	Opt<SourceLocation> source{};
	Opt<S> member_name{};
};

template<class Provider, class T>
concept JsonDumpProvider = requires(T const &value, DumpOptions const &opts) {
	{ Provider::dump_json(value, opts) } -> same_as<expected<S, Error>>;
};

template<class Provider, class T>
concept JsonDecodeProvider = requires(SV input, DecodeOptions const &opts) {
	{ Provider::template decode_json<T>(input, opts) } -> same_as<expected<T, Error>>;
};

template<class Provider>
concept JsonDocumentProvider = requires(SV input, DecodeOptions const &decode_opts, DumpOptions const &dump_opts) {
	typename Provider::document_type;
	{ Provider::parse_json_document(input, decode_opts) } -> same_as<expected<typename Provider::document_type, Error>>;
	{ Provider::dump_json(std::declval<typename Provider::document_type const &>(), dump_opts) } -> same_as<expected<S, Error>>;
};

template<class Provider, class T>
struct SerdeTraits {
	static expected<S, Error> dump(
		T const &value,
		DumpOptions const &opts = {})
		requires JsonDumpProvider<Provider, T>
	{
		return Provider::dump_json(value, opts);
	}

	static expected<T, Error> decode(
		SV input,
		DecodeOptions const &opts = {})
		requires JsonDecodeProvider<Provider, T>
	{
		return Provider::template decode_json<T>(input, opts);
	}
};

template<class Provider, class T>
[[nodiscard]] expected<S, Error> dump_with(
	T const &value,
	DumpOptions const &opts = {})
	requires JsonDumpProvider<Provider, T>
{
	return SerdeTraits<Provider, std::remove_cvref_t<T>>::dump(value, opts);
}

template<class Provider, class T>
[[nodiscard]] expected<std::remove_cvref_t<T>, Error> decode_with(
	SV input,
	DecodeOptions const &opts = {})
	requires JsonDecodeProvider<Provider, std::remove_cvref_t<T>>
{
	return SerdeTraits<Provider, std::remove_cvref_t<T>>::decode(input, opts);
}

} // namespace conflux::json::boundary
