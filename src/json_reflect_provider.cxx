export module conflux.json.reflect_provider;

import std;
import conflux.types;
import conflux.json;
export import conflux.json.boundary;
export import conflux.json.native_provider;
export import conflux.json.reflect;

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
			auto body = dump_reflect_direct<std::remove_cvref_t<T>>(
				value,
				JsonDumpOptions{
					.pretty = opts.pretty,
					.indent = opts.indent,
					.sort_object_keys = opts.sort_object_keys,
					.ascii_only = opts.ascii_only,
				});
			if (body) {
				return std::move(*body);
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
