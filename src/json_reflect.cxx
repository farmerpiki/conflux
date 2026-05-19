module;
export module conflux.json.reflect;
import std;
import conflux.json;
// ---------------------------------------------------------------------------
// Exported: annotation types for reflected structs
// ---------------------------------------------------------------------------

export namespace conflux::json {

struct name_t {
	char const *p;
	std::size_t n;
};
consteval name_t name(
	std::string_view sv) {
	return {std::define_static_string(sv), sv.size()};
}
struct skip {};

template<class T>
concept ReflectJsonAggregate =
	std::is_aggregate_v<T> && std::default_initializable<T> && (!requires { JsonMembers<T>::members(); });

} // namespace conflux::json
// ---------------------------------------------------------------------------
// Reflection helpers (consteval — require -freflection)
// ---------------------------------------------------------------------------

namespace detail {

template<std::meta::info Mem>
consteval bool reflect_has_skip() {
	return !std::meta::annotations_of_with_type(Mem, ^^conflux::json::skip).empty();
}
template<std::meta::info Mem>
consteval bool reflect_has_name() {
	return !std::meta::annotations_of_with_type(Mem, ^^conflux::json::name_t).empty();
}
template<std::meta::info Mem>
consteval conflux::json::name_t reflect_get_name_ann() {
	return std::meta::extract<conflux::json::name_t>(
		std::meta::annotations_of_with_type(Mem, ^^conflux::json::name_t)[0]);
}
template<class T>
consteval std::size_t reflect_member_count() {
	return std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()).size();
}
template<class T, std::size_t I>
consteval std::meta::info reflect_member_at() {
	return std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked())[I];
}
template<std::meta::info Mem>
consteval conflux::json::name_t reflect_field_name() {
	if constexpr (reflect_has_name<Mem>()) {
		return reflect_get_name_ann<Mem>();
	} else {
		auto sv = std::meta::identifier_of(Mem);
		return {std::define_static_string(sv), sv.size()};
	}
}
template<class T>
struct is_opt_refl : std::false_type {};
template<class T>
struct is_opt_refl<std::optional<T>> : std::true_type {};

inline void reflect_append_u_escape(
	std::string &out,
	std::uint32_t cp) {
	static constexpr std::array<char, 16> kHex =
		{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	out += "\\u";
	out += kHex[(cp >> 12U) & 0x0FU];
	out += kHex[(cp >> 8U) & 0x0FU];
	out += kHex[(cp >> 4U) & 0x0FU];
	out += kHex[cp & 0x0FU];
}

inline void reflect_dump_string(
	std::string &out,
	std::string_view sv,
	bool ascii_only) {
	out += '"';
	for (std::size_t i = 0; i < sv.size();) {
		auto const c = static_cast<unsigned char>(sv[i]);
		switch (c) {
		case '"':
			out += "\\\"";
			++i;
			break;
		case '\\':
			out += "\\\\";
			++i;
			break;
		case '\b':
			out += "\\b";
			++i;
			break;
		case '\f':
			out += "\\f";
			++i;
			break;
		case '\n':
			out += "\\n";
			++i;
			break;
		case '\r':
			out += "\\r";
			++i;
			break;
		case '\t':
			out += "\\t";
			++i;
			break;
		default:
			if (c < 0x20U) {
				reflect_append_u_escape(out, c);
				++i;
			} else if (ascii_only && c >= 0x80U) {
				std::uint32_t cp = 0;
				std::size_t seq = 0;
				if (c < 0xE0U) {
					cp = c & 0x1FU;
					seq = 2;
				} else if (c < 0xF0U) {
					cp = c & 0x0FU;
					seq = 3;
				} else {
					cp = c & 0x07U;
					seq = 4;
				}
				for (std::size_t k = 1; k < seq && i + k < sv.size(); ++k) {
					cp = (cp << 6U) | (static_cast<unsigned char>(sv[i + k]) & 0x3FU);
				}
				i += std::min(seq, sv.size() - i);
				if (cp < 0x10000U) {
					reflect_append_u_escape(out, cp);
				} else {
					cp -= 0x10000U;
					reflect_append_u_escape(out, 0xD800U | (cp >> 10U));
					reflect_append_u_escape(out, 0xDC00U | (cp & 0x3FFU));
				}
			} else {
				out += static_cast<char>(c);
				++i;
			}
		}
	}
	out += '"';
}

inline void reflect_indent(
	std::string &out,
	JsonDumpOptions const &opts,
	unsigned depth) {
	if (!opts.pretty) {
		return;
	}
	out += '\n';
	out.append(static_cast<std::size_t>(depth) * opts.indent, opts.indent_char);
}

[[nodiscard]] inline std::expected<void, JsonError> skip_reader_event(
	JsonReader &reader,
	JsonReader::Event event) {
	using Ev = JsonReader::Event;
	if (event == Ev::string_value || event == Ev::number_value || event == Ev::bool_value || event == Ev::null_value) {
		return {};
	}
	int depth = 1;
	while (depth > 0) {
		auto next = reader.next();
		if (!next) {
			return std::unexpected(std::move(next).error());
		}
		if (!*next) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::unexpected_eof,
					.message = "EOF while skipping"});
		}
		if (**next == Ev::begin_object || **next == Ev::begin_array) {
			++depth;
		} else if (**next == Ev::end_object || **next == Ev::end_array) {
			--depth;
		}
	}
	return {};
}

[[nodiscard]] inline std::expected<std::string_view, JsonError> reflect_key_view(
	JsonStringToken const &token,
	JsonDecodeScratch &scratch) {
	if (auto borrowed = token.unescaped_borrow()) {
		return *borrowed;
	}
	auto const needed = token.max_decoded_size();
	if (needed <= scratch.key_inline.size()) {
		return token.decode_into(std::span<char>{scratch.key_inline.data(), scratch.key_inline.size()});
	}
	scratch.key_overflow.resize(needed);
	return token.decode_into(std::span<char>{scratch.key_overflow.data(), scratch.key_overflow.size()});
}

template<class M>
[[nodiscard]] std::expected<M, JsonError> decode_reflect_reader_member(
	JsonReader &reader,
	JsonReader::Event event,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	using Ev = JsonReader::Event;
	using Raw = std::remove_cvref_t<M>;
	if constexpr (conflux::json::ReflectJsonAggregate<Raw>) {
		return JsonCodec<Raw>::decode(reader, event, opts, scratch);
	} else if constexpr (std::same_as<Raw, bool>) {
		if (event != Ev::bool_value) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected bool"});
		}
		return reader.bool_val();
	} else if constexpr (std::same_as<Raw, std::string>) {
		if (event != Ev::string_value) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected string"});
		}
		std::string out;
		if (auto ok = reader.string_token().append_decoded_to(out); !ok) {
			return std::unexpected(std::move(ok).error());
		}
		return out;
	} else if constexpr (is_opt_refl<Raw>::value) {
		using Inner = typename Raw::value_type;
		if (event == Ev::null_value) {
			return Raw{};
		}
		auto decoded = decode_reflect_reader_member<Inner>(reader, event, opts, scratch);
		if (!decoded) {
			return std::unexpected(std::move(decoded).error());
		}
		return Raw{std::move(*decoded)};
	} else if constexpr (std::is_signed_v<Raw> && std::integral<Raw>) {
		if (event != Ev::number_value) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected number"});
		}
		auto n = reader.number_val().to_i64();
		if (!n) {
			return std::unexpected(std::move(n).error());
		}
		if (*n < static_cast<std::int64_t>(std::numeric_limits<Raw>::min())
			|| *n > static_cast<std::int64_t>(std::numeric_limits<Raw>::max())) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::number_out_of_range,
					.message = "integer out of range"});
		}
		return static_cast<Raw>(*n);
	} else if constexpr (std::is_unsigned_v<Raw> && std::integral<Raw>) {
		if (event != Ev::number_value) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected number"});
		}
		auto n = reader.number_val().to_u64();
		if (!n) {
			return std::unexpected(std::move(n).error());
		}
		if (*n > static_cast<std::uint64_t>(std::numeric_limits<Raw>::max())) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::number_out_of_range,
					.message = "integer out of range"});
		}
		return static_cast<Raw>(*n);
	} else if constexpr (std::floating_point<Raw>) {
		if (event != Ev::number_value) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected number"});
		}
		auto n = reader.number_val().to_f64();
		if (!n) {
			return std::unexpected(std::move(n).error());
		}
		return static_cast<Raw>(*n);
	} else {
		static_assert(!std::same_as<Raw, Raw>, "no reader decode support for reflected member type");
	}
}
// Decode a NodeRef into M, handling non-codec integral/float types.
template<class M>
[[nodiscard]] std::expected<M, JsonError> decode_reflect_member(
	NodeRef node,
	JsonDecodeOptions const &opts) {
	if constexpr (has_json_codec<M>) {
		return ::decode<M>(node, opts);
	} else if constexpr (std::is_signed_v<M> && std::integral<M>) {
		auto r = JsonCodec<std::int64_t>::decode(node);
		if (!r) {
			return std::unexpected(std::move(r).error());
		}
		if (*r < static_cast<std::int64_t>(std::numeric_limits<M>::min())
			|| *r > static_cast<std::int64_t>(std::numeric_limits<M>::max())) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::number_out_of_range,
					.message =
						std::format("value out of std::int64_t range for {}", std::meta::display_string_of(^^M))});
		}
		return static_cast<M>(*r);
	} else if constexpr (std::is_unsigned_v<M> && std::integral<M>) {
		auto r = JsonCodec<std::uint64_t>::decode(node);
		if (!r) {
			return std::unexpected(std::move(r).error());
		}
		if (*r > static_cast<std::uint64_t>(std::numeric_limits<M>::max())) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::number_out_of_range,
					.message =
						std::format("value out of std::uint64_t range for {}", std::meta::display_string_of(^^M))});
		}
		return static_cast<M>(*r);
	} else if constexpr (std::floating_point<M>) {
		auto r = JsonCodec<double>::decode(node);
		if (!r) {
			return std::unexpected(std::move(r).error());
		}
		return static_cast<M>(*r);
	} else {
		static_assert(!std::same_as<M, M>, "no decode support for reflected member type");
	}
}
// Encode M into ObjectBuilder, handling non-codec integral/float types.
template<class M>
[[nodiscard]] std::expected<void, JsonError> encode_reflect_member(
	ObjectBuilder &obj,
	std::string_view name,
	M const &value) {
	if constexpr (requires { JsonCodec<M>::encode(std::declval<ValueBuilder &>(), std::declval<M const &>()); }) {
		return obj.template insert<M>(name, value);
	} else if constexpr (std::is_signed_v<M> && std::integral<M>) {
		return obj.insert_i64(name, static_cast<std::int64_t>(value));
	} else if constexpr (std::is_unsigned_v<M> && std::integral<M>) {
		return obj.insert_u64(name, static_cast<std::uint64_t>(value));
	} else if constexpr (std::floating_point<M>) {
		return obj.insert_f64(name, static_cast<double>(value));
	} else if constexpr (std::convertible_to<M, std::string_view>) {
		return obj.insert_string(name, static_cast<std::string_view>(value));
	} else {
		static_assert(!std::same_as<M, M>, "no encode support for reflected member type");
	}
}

} // namespace detail
// ---------------------------------------------------------------------------
// JsonCodec<T> partial specialization — reflection-derived encode / decode
// ---------------------------------------------------------------------------

template<class T>
	requires conflux::json::ReflectJsonAggregate<T>
struct JsonCodec<T> {
	static std::expected<T, JsonError> decode(
		NodeRef root) {
		return decode(root, {});
	}

	static std::expected<T, JsonError> decode(
		NodeRef root,
		JsonDecodeOptions const &opts) {
		auto obj_res = root.as_object();
		if (!obj_res) {
			return std::unexpected(std::move(obj_res).error());
		}
		auto const &obj = *obj_res;

		T result{};
		bool ok = true;
		JsonError first_err;

		constexpr auto N = detail::reflect_member_count<T>();

		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(
				[&]<std::size_t I>() {
					if (!ok) {
						return;
					}
					constexpr auto mem = detail::reflect_member_at<T, I>();
					if constexpr (detail::reflect_has_skip<mem>()) {
						return;
					}

					constexpr auto name_info = detail::reflect_field_name<mem>();
					std::string_view const field_name{name_info.p, name_info.n};

					using M = std::remove_cvref_t<decltype(result.[:mem:])>;
					auto node = obj.find_member(field_name);
					if (!node) {
						if constexpr (!detail::is_opt_refl<M>::value) {
							ok = false;
							first_err = JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::missing_member,
								.member_name = std::string{field_name},
								.message = std::format("missing member: {}", field_name)};
						}
						return;
					}
					auto decoded = detail::decode_reflect_member<M>(*node, opts);
					if (!decoded) {
						ok = false;
						first_err = std::move(decoded).error();
						return;
					}
					result.[:mem:] = std::move(*decoded);
				}.template operator ()<Is>(),
				...);
		}(std::make_index_sequence<N>{});

		if (!ok) {
			return std::unexpected(std::move(first_err));
		}

		// Unknown-member handling follows JsonDecodeOptions so reflected serde
		// behaves like manual JsonMembers<T> codecs at app/provider boundaries.
		if (opts.unknown_members == UnknownMemberPolicy::reject) {
			for (auto const &m: obj.members()) {
				if (!ok) {
					break;
				}
				bool found = false;
				[&]<std::size_t... Is>(std::index_sequence<Is...>) {
					(
						[&]<std::size_t I>() {
							if (found) {
								return;
							}
							constexpr auto mem = detail::reflect_member_at<T, I>();
							if constexpr (detail::reflect_has_skip<mem>()) {
								return;
							}
							constexpr auto ni = detail::reflect_field_name<mem>();
							if (std::string_view{ni.p, ni.n} == m.name) {
								found = true;
							}
						}.template operator ()<Is>(),
						...);
				}(std::make_index_sequence<N>{});
				if (!found) {
					ok = false;
					first_err = JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.member_name = std::string{m.name},
						.message = std::format("unknown member: {}", m.name)};
				}
			}
		}

		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		return result;
	}
	static std::expected<T, JsonError> decode(
		JsonReader &reader,
		JsonReader::Event event,
		JsonDecodeOptions const &opts,
		JsonDecodeScratch *scratch) {
		using Ev = JsonReader::Event;
		if (event != Ev::begin_object) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected object"});
		}

		T result{};
		bool ok = true;
		JsonError first_err;
		constexpr auto N = detail::reflect_member_count<T>();
		std::array<bool, N> found{};
		JsonDecodeScratch local_scratch;
		JsonDecodeScratch &decode_scratch = scratch != nullptr ? *scratch : local_scratch;

		while (ok) {
			auto next = reader.next();
			if (!next) {
				ok = false;
				first_err = std::move(next).error();
				break;
			}
			if (!*next) {
				ok = false;
				first_err = JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::unexpected_eof,
					.message = "EOF in object"};
				break;
			}
			if (**next == Ev::end_object) {
				break;
			}
			if (**next != Ev::key) {
				ok = false;
				first_err = JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::syntax_error,
					.message = "expected key"};
				break;
			}

			auto key_res = detail::reflect_key_view(reader.key_token(), decode_scratch);
			if (!key_res) {
				ok = false;
				first_err = std::move(key_res).error();
				break;
			}
			std::string_view const key = *key_res;
			bool matched = false;

			[&]<std::size_t... Is>(std::index_sequence<Is...>) {
				(
					[&]<std::size_t I>() {
						if (matched || !ok) {
							return;
						}
						constexpr auto mem = detail::reflect_member_at<T, I>();
						if constexpr (detail::reflect_has_skip<mem>()) {
							return;
						}
						constexpr auto name_info = detail::reflect_field_name<mem>();
						std::string_view const field_name{name_info.p, name_info.n};
						if (key != field_name) {
							return;
						}
						matched = true;
						found[I] = true;

						auto value = reader.next();
						if (!value) {
							ok = false;
							first_err = std::move(value).error();
							return;
						}
						if (!*value) {
							ok = false;
							first_err = JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::unexpected_eof,
								.message = "EOF in object value"};
							return;
						}

						using M = std::remove_cvref_t<decltype(result.[:mem:])>;
						auto decoded = detail::decode_reflect_reader_member<M>(reader, **value, opts, &decode_scratch);
						if (!decoded) {
							ok = false;
							first_err = std::move(decoded).error();
							return;
						}
						result.[:mem:] = std::move(*decoded);
					}.template operator ()<Is>(),
					...);
			}(std::make_index_sequence<N>{});

			if (!matched && ok) {
				if (opts.unknown_members == UnknownMemberPolicy::reject) {
					ok = false;
					first_err = JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.member_name = std::string{key},
						.message = std::format("unknown member: {}", key)};
				} else {
					auto value = reader.next();
					if (!value) {
						ok = false;
						first_err = std::move(value).error();
					} else if (!*value) {
						ok = false;
						first_err = JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::unexpected_eof,
							.message = "EOF in object value"};
					} else if (auto skipped = detail::skip_reader_event(reader, **value); !skipped) {
						ok = false;
						first_err = std::move(skipped).error();
					}
				}
			}
		}

		if (!ok) {
			return std::unexpected(std::move(first_err));
		}

		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(
				[&]<std::size_t I>() {
					if (!ok) {
						return;
					}
					constexpr auto mem = detail::reflect_member_at<T, I>();
					if constexpr (detail::reflect_has_skip<mem>()) {
						return;
					}
					using M = std::remove_cvref_t<decltype(result.[:mem:])>;
					if (!found[I] && !detail::is_opt_refl<M>::value) {
						constexpr auto name_info = detail::reflect_field_name<mem>();
						std::string_view const field_name{name_info.p, name_info.n};
						ok = false;
						first_err = JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::missing_member,
							.member_name = std::string{field_name},
							.message = std::format("missing member: {}", field_name)};
					}
				}.template operator ()<Is>(),
				...);
		}(std::make_index_sequence<N>{});

		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		return result;
	}
	static std::expected<void, JsonError> encode(
		ValueBuilder &b,
		T const &value) {
		auto obj_res = b.begin_object();
		if (!obj_res) {
			return std::unexpected(std::move(obj_res).error());
		}
		auto &obj = *obj_res;

		bool ok = true;
		JsonError first_err;

		constexpr auto N = detail::reflect_member_count<T>();
		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(
				[&]<std::size_t I>() {
					if (!ok) {
						return;
					}
					constexpr auto mem = detail::reflect_member_at<T, I>();
					if constexpr (detail::reflect_has_skip<mem>()) {
						return;
					}

					constexpr auto name_info = detail::reflect_field_name<mem>();
					std::string_view const field_name{name_info.p, name_info.n};

					using M = std::remove_cvref_t<decltype(value.[:mem:])>;
					auto res = detail::encode_reflect_member<M>(obj, field_name, value.[:mem:]);
					if (!res) {
						ok = false;
						first_err = std::move(res).error();
					}
				}.template operator ()<Is>(),
				...);
		}(std::make_index_sequence<N>{});

		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		std::move(obj).commit();
		return {};
	}
	static constexpr std::string_view type_name() { return std::meta::display_string_of(^^T); }
};

namespace detail {

template<class T>
std::expected<void, JsonError>
reflect_write_value(std::string &out, T const &value, JsonDumpOptions const &opts, unsigned depth);

template<class T>
std::expected<void, JsonError> reflect_write_array_like(
	std::string &out,
	T const &value,
	JsonDumpOptions const &opts,
	unsigned depth) {
	out += '[';
	bool first = true;
	for (auto const &elem: value) {
		if (!first) {
			out += ',';
		}
		if (opts.pretty) {
			reflect_indent(out, opts, depth + 1);
		}
		if (auto ok = reflect_write_value(out, elem, opts, depth + 1); !ok) {
			return ok;
		}
		first = false;
	}
	if (opts.pretty && !first) {
		reflect_indent(out, opts, depth);
	}
	out += ']';
	return {};
}

template<class T>
std::expected<void, JsonError> reflect_write_object(
	std::string &out,
	T const &value,
	JsonDumpOptions const &opts,
	unsigned depth) {
	if (opts.sort_object_keys) {
		return std::unexpected(
			JsonError{
				.stage = JsonStage::dump,
				.code = JsonIssueCode::invalid_value,
				.message = "reflected direct writer does not support sort_object_keys"});
	}
	out += '{';
	bool ok = true;
	bool first = true;
	JsonError first_err;
	constexpr auto N = reflect_member_count<T>();
	[&]<std::size_t... Is>(std::index_sequence<Is...>) {
		(
			[&]<std::size_t I>() {
				if (!ok) {
					return;
				}
				constexpr auto mem = reflect_member_at<T, I>();
				if constexpr (reflect_has_skip<mem>()) {
					return;
				}
				constexpr auto name_info = reflect_field_name<mem>();
				std::string_view const field_name{name_info.p, name_info.n};
				if (!first) {
					out += ',';
				}
				if (opts.pretty) {
					reflect_indent(out, opts, depth + 1);
				}
				reflect_dump_string(out, field_name, opts.ascii_only);
				out += opts.pretty ? ": " : ":";
				if (auto res = reflect_write_value(out, value.[:mem:], opts, depth + 1); !res) {
					ok = false;
					first_err = std::move(res).error();
					first_err.member_name = std::string{field_name};
					return;
				}
				first = false;
			}.template operator ()<Is>(),
			...);
	}(std::make_index_sequence<N>{});
	if (!ok) {
		return std::unexpected(std::move(first_err));
	}
	if (opts.pretty && !first) {
		reflect_indent(out, opts, depth);
	}
	out += '}';
	return {};
}

template<class T>
std::expected<void, JsonError> reflect_write_value(
	std::string &out,
	T const &value,
	JsonDumpOptions const &opts,
	unsigned depth) {
	using Raw = std::remove_cvref_t<T>;
	if constexpr (conflux::json::ReflectJsonAggregate<Raw>) {
		return reflect_write_object(out, value, opts, depth);
	} else if constexpr (std::same_as<Raw, bool>) {
		out += value ? "true" : "false";
		return {};
	} else if constexpr (std::same_as<Raw, std::string>) {
		reflect_dump_string(out, value, opts.ascii_only);
		return {};
	} else if constexpr (std::same_as<Raw, std::string_view>) {
		reflect_dump_string(out, value, opts.ascii_only);
		return {};
	} else if constexpr ((std::integral<Raw> && !std::same_as<Raw, bool>) || std::floating_point<Raw>) {
		std::array<char, 64> buf{};
		auto *first = buf.data();
		auto *last = buf.data() + buf.size();
		auto [ptr, ec] = std::to_chars(first, last, value);
		if (ec != std::errc{}) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::dump,
					.code = JsonIssueCode::invalid_number,
					.message = "number formatting failed"});
		}
		out.append(first, static_cast<std::size_t>(ptr - first));
		return {};
	} else if constexpr (is_opt_refl<Raw>::value) {
		if (!value) {
			out += "null";
			return {};
		}
		return reflect_write_value(out, *value, opts, depth);
	} else if constexpr (requires { write_json_direct(out, value, opts); }) {
		return write_json_direct(out, value, opts);
	} else if constexpr (requires {
							 value.begin();
							 value.end();
						 }) {
		return reflect_write_array_like(out, value, opts, depth);
	} else {
		static_assert(!std::same_as<Raw, Raw>, "no reflected direct writer support for member type");
	}
}

} // namespace detail

export template<conflux::json::ReflectJsonAggregate T>
std::expected<void, JsonError> write_reflect_json_direct(
	std::string &out,
	T const &value,
	JsonDumpOptions const &opts = {}) {
	return detail::reflect_write_object(out, value, opts, 0);
}

export template<conflux::json::ReflectJsonAggregate T>
std::expected<std::string, JsonError> dump_reflect_direct(
	T const &value,
	JsonDumpOptions const &opts = {}) {
	std::string out;
	out.reserve(detail::reflect_member_count<T>() * 16);
	if (auto ok = write_reflect_json_direct(out, value, opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	return out;
}
