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
concept ReflectJsonAggregate = std::is_aggregate_v<T>
							&& std::default_initializable<T>
							&& (!requires { conflux::json::JsonMembers<T>::members(); });

} // namespace conflux::json
// ---------------------------------------------------------------------------
// Reflection helpers (consteval — require -freflection)
// ---------------------------------------------------------------------------

namespace detail {

using conflux::json::DuplicateKeyPolicy;
using conflux::json::has_json_codec;
using conflux::json::JsonCodec;
using conflux::json::JsonDecodeOptions;
using conflux::json::JsonDecodeScratch;
using conflux::json::JsonDumpOptions;
using conflux::json::JsonError;
using conflux::json::JsonIssueCode;
using conflux::json::JsonPath;
using conflux::json::JsonReader;
using conflux::json::JsonSourceLocation;
using conflux::json::JsonStage;
using conflux::json::JsonStringToken;
using conflux::json::NodeRef;
using conflux::json::ObjectBuilder;
using conflux::json::ParseMode;
using conflux::json::UnknownMemberPolicy;
using conflux::json::ValueBuilder;

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
template<class T>
struct is_basic_string_refl : std::false_type {};
template<class Traits, class Alloc>
struct is_basic_string_refl<std::basic_string<char, Traits, Alloc>> : std::true_type {};
template<class T>
struct is_vector_refl : std::false_type {};
template<class T, class Alloc>
struct is_vector_refl<std::vector<T, Alloc>> : std::true_type {};
template<class T>
struct is_array_refl : std::false_type {};
template<class T, std::size_t N>
struct is_array_refl<std::array<T, N>> : std::true_type {};

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

template<ParseMode Mode>
[[nodiscard]] inline std::expected<void, JsonError> skip_reader_event(
	JsonReader &reader,
	JsonReader::Event event) {
	using Ev = JsonReader::Event;
	if (event == Ev::string_value || event == Ev::number_value || event == Ev::bool_value || event == Ev::null_value) {
		return {};
	}
	int depth = 1;
	while (depth > 0) {
		auto next = reader.next_impl<Mode>();
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

template<ParseMode Mode, class Vector>
[[nodiscard]] inline std::expected<void, JsonError> reflect_reserve_vector_from_remaining_array(
	Vector &out,
	JsonReader &reader) {
	auto count = reader.count_remaining_array_elements_impl<Mode>();
	if (!count) {
		return std::unexpected(std::move(count).error());
	}
	out.reserve(*count);
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

[[nodiscard]] inline JsonError reflect_duplicate_member_error(
	std::string_view name) {
	return JsonError{
		.stage = JsonStage::decode,
		.code = JsonIssueCode::duplicate_member,
		.member_name = std::string{name},
		.message = std::format("duplicate member: {}", name)};
}

template<class String>
[[nodiscard]] inline std::expected<void, JsonError> reflect_decode_string_into(
	String &out,
	JsonStringToken const &token,
	JsonDecodeScratch *scratch = nullptr) {
	if (auto borrowed = token.unescaped_borrow()) {
		out.assign(borrowed->data(), borrowed->size());
		return {};
	}
	auto const needed = token.max_decoded_size();
	if (scratch != nullptr && needed <= scratch->string_inline.size()) {
		auto res = token.decode_into(std::span<char>{scratch->string_inline.data(), scratch->string_inline.size()});
		if (!res) {
			out.clear();
			return std::unexpected(std::move(res).error());
		}
		out.assign(res->data(), res->size());
		return {};
	}
	out.clear();
	out.resize(needed);
	auto res = token.decode_into(std::span<char>{out.data(), out.size()});
	if (!res) {
		out.clear();
		return std::unexpected(std::move(res).error());
	}
	out.resize(res->size());
	return {};
}

template<ParseMode Mode, class T>
[[nodiscard]] std::expected<void, JsonError> decode_reflect_reader_aggregate_into(
	T &result,
	JsonReader &reader,
	JsonReader::Event event,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch);

template<ParseMode Mode, class M>
[[nodiscard]] std::expected<void, JsonError> decode_reflect_reader_member_into(
	M &out,
	JsonReader &reader,
	JsonReader::Event event,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch);

// Wide-object measurements for the manual JsonMembers path showed generated
// lookup wins from 16 fields upward while the tiny linear path stays best for
// small aggregates. Keep reflection on the same cutoff.
inline constexpr std::size_t kReflectMemberLinearLookupLimit = 8;

[[nodiscard]] constexpr std::uint64_t reflect_member_name_hash(
	std::string_view name) noexcept {
	std::uint64_t h = 1469598103934665603ULL;
	for (char c: name) {
		h ^= static_cast<unsigned char>(c);
		h *= 1099511628211ULL;
	}
	return h;
}

[[nodiscard]] constexpr std::size_t reflect_member_lookup_capacity(
	std::size_t member_count) noexcept {
	std::size_t capacity = 1;
	while (capacity < member_count * 2) {
		capacity <<= 1;
	}
	return capacity;
}

template<class T>
using ReflectMemberDecodeFn = std::expected<void, JsonError> (
		*)(T &, JsonReader &, JsonReader::Event, JsonDecodeOptions const &, JsonDecodeScratch *);

template<class T>
struct ReflectMemberLookupEntry {
	std::string_view name{};
	std::uint64_t hash{};
	std::size_t index{};
	ReflectMemberDecodeFn<T> decode_strict{};
	ReflectMemberDecodeFn<T> decode_json5{};
	bool occupied{};
};

template<ParseMode Mode, class T, std::size_t I>
std::expected<void, JsonError> decode_reflect_member_by_static_index(
	T &result,
	JsonReader &reader,
	JsonReader::Event event,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	constexpr auto mem = reflect_member_at<T, I>();
	auto decoded = decode_reflect_reader_member_into<Mode>(result.[:mem:], reader, event, opts, scratch);
	if (!decoded) {
		return std::unexpected(std::move(decoded).error());
	}
	return {};
}

template<class T, std::size_t I>
[[nodiscard]] ReflectMemberLookupEntry<T> make_reflect_member_lookup_entry() {
	constexpr auto mem = reflect_member_at<T, I>();
	constexpr auto name_info = reflect_field_name<mem>();
	std::string_view const field_name{name_info.p, name_info.n};
	return ReflectMemberLookupEntry<T>{
		.name = field_name,
		.hash = reflect_member_name_hash(field_name),
		.index = I,
		.decode_strict = &decode_reflect_member_by_static_index<ParseMode::strict, T, I>,
		.decode_json5 = &decode_reflect_member_by_static_index<ParseMode::json5, T, I>,
		.occupied = true};
}

template<class T, std::size_t... Is>
[[nodiscard]] auto make_reflect_member_lookup_slots_impl(
	std::index_sequence<Is...>) {
	constexpr std::size_t member_count = sizeof...(Is);
	std::array<ReflectMemberLookupEntry<T>, reflect_member_lookup_capacity(member_count)> slots{};
	auto insert = [&](ReflectMemberLookupEntry<T> entry) {
		auto pos = static_cast<std::size_t>(entry.hash) & (slots.size() - 1);
		while (slots[pos].occupied) {
			pos = (pos + 1) & (slots.size() - 1);
		}
		slots[pos] = entry;
	};
	(
		[&]<std::size_t I>() {
			constexpr auto mem = reflect_member_at<T, I>();
			if constexpr (!reflect_has_skip<mem>()) {
				insert(make_reflect_member_lookup_entry<T, I>());
			}
		}.template operator ()<Is>(),
		...);
	return slots;
}

template<class T>
[[nodiscard]] auto const &reflect_member_lookup_slots() {
	constexpr std::size_t member_count = reflect_member_count<T>();
	static auto const slots = make_reflect_member_lookup_slots_impl<T>(std::make_index_sequence<member_count>{});
	return slots;
}

template<class T>
[[nodiscard]] ReflectMemberLookupEntry<T> const *find_reflect_member_lookup_entry(
	std::string_view key) {
	auto const &slots = reflect_member_lookup_slots<T>();
	std::uint64_t const hash = reflect_member_name_hash(key);
	auto pos = static_cast<std::size_t>(hash) & (slots.size() - 1);
	for (std::size_t probe = 0; probe < slots.size(); ++probe) {
		auto const &slot = slots[pos];
		if (!slot.occupied) {
			return nullptr;
		}
		if (slot.hash == hash && slot.name == key) {
			return &slot;
		}
		pos = (pos + 1) & (slots.size() - 1);
	}
	return nullptr;
}

template<class T>
[[nodiscard]] bool is_reflect_member_name(
	std::string_view key) {
	constexpr auto N = reflect_member_count<T>();
	if constexpr (N > kReflectMemberLinearLookupLimit) {
		return find_reflect_member_lookup_entry<T>(key) != nullptr;
	} else {
		bool found = false;
		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(
				[&]<std::size_t I>() {
					if (found) {
						return;
					}
					constexpr auto mem = reflect_member_at<T, I>();
					if constexpr (reflect_has_skip<mem>()) {
						return;
					}
					constexpr auto name_info = reflect_field_name<mem>();
					found = key == std::string_view{name_info.p, name_info.n};
				}.template operator ()<Is>(),
				...);
		}(std::make_index_sequence<N>{});
		return found;
	}
}

template<ParseMode Mode, class M>
[[nodiscard]] std::expected<M, JsonError> decode_reflect_reader_member(
	JsonReader &reader,
	JsonReader::Event event,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	M result{};
	if (auto decoded = decode_reflect_reader_member_into<Mode>(result, reader, event, opts, scratch); !decoded) {
		return std::unexpected(std::move(decoded).error());
	}
	return result;
}

template<ParseMode Mode, class M>
[[nodiscard]] std::expected<void, JsonError> decode_reflect_reader_member_into(
	M &out,
	JsonReader &reader,
	JsonReader::Event event,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	using Ev = JsonReader::Event;
	using Raw = std::remove_cvref_t<M>;
	if constexpr (is_vector_refl<Raw>::value) {
		using Elem = typename Raw::value_type;
		if (event != Ev::begin_array) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected array"});
		}
		out.clear();
		if (auto reserved = reflect_reserve_vector_from_remaining_array<Mode>(out, reader); !reserved) {
			return std::unexpected(std::move(reserved).error());
		}
		while (true) {
			auto next = reader.next_impl<Mode>();
			if (!next) {
				return std::unexpected(std::move(next).error());
			}
			if (!*next) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in array"});
			}
			if (**next == Ev::end_array) {
				return {};
			}
			if constexpr (std::default_initializable<Elem>) {
				auto &slot = out.emplace_back();
				auto elem = decode_reflect_reader_member_into<Mode>(slot, reader, **next, opts, scratch);
				if (!elem) {
					return std::unexpected(std::move(elem).error());
				}
			} else {
				auto elem = decode_reflect_reader_member<Mode, Elem>(reader, **next, opts, scratch);
				if (!elem) {
					return std::unexpected(std::move(elem).error());
				}
				out.push_back(std::move(*elem));
			}
		}
	} else if constexpr (is_array_refl<Raw>::value) {
		constexpr std::size_t N = std::tuple_size_v<Raw>;
		if (event != Ev::begin_array) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected array"});
		}
		for (std::size_t i = 0; i < N; ++i) {
			auto next = reader.next_impl<Mode>();
			if (!next) {
				return std::unexpected(std::move(next).error());
			}
			if (!*next) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in array"});
			}
			if (**next == Ev::end_array) {
				return std::unexpected(
					JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.message = std::format("expected array of length {}", N)});
			}
			auto elem = decode_reflect_reader_member_into<Mode>(out[i], reader, **next, opts, scratch);
			if (!elem) {
				return std::unexpected(std::move(elem).error());
			}
		}
		auto next = reader.next_impl<Mode>();
		if (!next) {
			return std::unexpected(std::move(next).error());
		}
		if (!*next || **next != Ev::end_array) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.message = std::format("expected array of length {}", N)});
		}
		return {};
	} else if constexpr (conflux::json::ReflectJsonAggregate<Raw>) {
		return decode_reflect_reader_aggregate_into<Mode>(out, reader, event, opts, scratch);
	} else if constexpr (std::same_as<Raw, bool>) {
		if (event != Ev::bool_value) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected bool"});
		}
		out = reader.bool_val();
		return {};
	} else if constexpr (is_basic_string_refl<Raw>::value) {
		if (event != Ev::string_value) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected string"});
		}
		return reflect_decode_string_into(out, reader.string_token(), scratch);
	} else if constexpr (is_opt_refl<Raw>::value) {
		using Inner = typename Raw::value_type;
		if (event == Ev::null_value) {
			out.reset();
			return {};
		}
		if constexpr (std::default_initializable<Inner>) {
			out.emplace();
			auto decoded = decode_reflect_reader_member_into<Mode>(*out, reader, event, opts, scratch);
			if (!decoded) {
				out.reset();
				return std::unexpected(std::move(decoded).error());
			}
			return {};
		} else {
			auto decoded = decode_reflect_reader_member<Mode, Inner>(reader, event, opts, scratch);
			if (!decoded) {
				out.reset();
				return std::unexpected(std::move(decoded).error());
			}
			out.emplace(std::move(*decoded));
			return {};
		}
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
		out = static_cast<Raw>(*n);
		return {};
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
		out = static_cast<Raw>(*n);
		return {};
	} else if constexpr (std::floating_point<Raw>) {
		if (event != Ev::number_value) {
			return std::unexpected(
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected number"});
		}
		auto n = reader.number_val().to_f64();
		if (!n) {
			return std::unexpected(std::move(n).error());
		}
		out = static_cast<Raw>(*n);
		return {};
	} else {
		static_assert(!std::same_as<Raw, Raw>, "no reader decode support for reflected member type");
	}
}

template<ParseMode Mode, class T>
[[nodiscard]] std::expected<void, JsonError> decode_reflect_reader_aggregate_into(
	T &result,
	JsonReader &reader,
	JsonReader::Event event,
	JsonDecodeOptions const &opts,
	JsonDecodeScratch *scratch) {
	using Ev = JsonReader::Event;
	if (event != Ev::begin_object) {
		return std::unexpected(
			JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::wrong_kind, .message = "expected object"});
	}

	bool ok = true;
	JsonError first_err;
	constexpr auto N = reflect_member_count<T>();
	std::array<bool, N> found{};
	JsonDecodeScratch local_scratch;
	JsonDecodeScratch &decode_scratch = scratch != nullptr ? *scratch : local_scratch;

	while (ok) {
		auto next = reader.next_impl<Mode>();
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
			first_err =
				JsonError{.stage = JsonStage::decode, .code = JsonIssueCode::syntax_error, .message = "expected key"};
			break;
		}

		auto key_res = reflect_key_view(reader.key_token(), decode_scratch);
		if (!key_res) {
			ok = false;
			first_err = std::move(key_res).error();
			break;
		}
		std::string_view const key = *key_res;
		bool matched = false;

		if constexpr (N > kReflectMemberLinearLookupLimit) {
			if (auto const *entry = find_reflect_member_lookup_entry<T>(key); entry != nullptr) {
				matched = true;
				bool const already_found = found[entry->index];
				if (already_found && reader.parse_options().duplicate_key == DuplicateKeyPolicy::reject) {
					ok = false;
					first_err = reflect_duplicate_member_error(entry->name);
				} else {
					auto value = reader.next_impl<Mode>();
					if (!value) {
						ok = false;
						first_err = std::move(value).error();
					} else if (!*value) {
						ok = false;
						first_err = JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::unexpected_eof,
							.message = "EOF in object value"};
					} else if (
						already_found && reader.parse_options().duplicate_key == DuplicateKeyPolicy::first_wins) {
						if (auto skipped = skip_reader_event<Mode>(reader, **value); !skipped) {
							ok = false;
							first_err = std::move(skipped).error();
						}
					} else {
						if (!already_found) {
							found[entry->index] = true;
						}
						auto decoded = [&]() {
							if constexpr (Mode == ParseMode::strict) {
								return entry->decode_strict(result, reader, **value, opts, &decode_scratch);
							} else {
								return entry->decode_json5(result, reader, **value, opts, &decode_scratch);
							}
						}();
						if (!decoded) {
							ok = false;
							first_err = std::move(decoded).error();
						}
					}
				}
			}
		} else {
			[&]<std::size_t... Is>(std::index_sequence<Is...>) {
				(
					[&]<std::size_t I>() {
						if (matched || !ok) {
							return;
						}
						constexpr auto mem = reflect_member_at<T, I>();
						if constexpr (reflect_has_skip<mem>()) {
							return;
						}
						constexpr auto name_info = reflect_field_name<mem>();
						std::string_view const field_name{name_info.p, name_info.n};
						if (key != field_name) {
							return;
						}
						matched = true;
						bool const already_found = found[I];
						if (already_found && reader.parse_options().duplicate_key == DuplicateKeyPolicy::reject) {
							ok = false;
							first_err = reflect_duplicate_member_error(field_name);
							return;
						}

						auto value = reader.next_impl<Mode>();
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
						if (already_found && reader.parse_options().duplicate_key == DuplicateKeyPolicy::first_wins) {
							if (auto skipped = skip_reader_event<Mode>(reader, **value); !skipped) {
								ok = false;
								first_err = std::move(skipped).error();
							}
							return;
						}
						if (!already_found) {
							found[I] = true;
						}

						auto decoded = decode_reflect_reader_member_into<Mode>(
							result.[:mem:], reader, **value, opts, &decode_scratch);
						if (!decoded) {
							ok = false;
							first_err = std::move(decoded).error();
							return;
						}
					}.template operator ()<Is>(),
					...);
			}(std::make_index_sequence<N>{});
		}

		if (!matched && ok) {
			if (opts.unknown_members == UnknownMemberPolicy::reject) {
				ok = false;
				first_err = JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.member_name = std::string{key},
					.message = std::format("unknown member: {}", key)};
			} else {
				auto value = reader.next_impl<Mode>();
				if (!value) {
					ok = false;
					first_err = std::move(value).error();
				} else if (!*value) {
					ok = false;
					first_err = JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::unexpected_eof,
						.message = "EOF in object value"};
				} else if (auto skipped = skip_reader_event<Mode>(reader, **value); !skipped) {
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
				constexpr auto mem = reflect_member_at<T, I>();
				if constexpr (reflect_has_skip<mem>()) {
					return;
				}
				using M = std::remove_cvref_t<decltype(result.[:mem:])>;
				if (!found[I]) {
					if constexpr (is_opt_refl<M>::value) {
						result.[:mem:].reset();
					} else {
						constexpr auto name_info = reflect_field_name<mem>();
						std::string_view const field_name{name_info.p, name_info.n};
						ok = false;
						first_err = JsonError{
							.stage = JsonStage::decode,
							.code = JsonIssueCode::missing_member,
							.member_name = std::string{field_name},
							.message = std::format("missing member: {}", field_name)};
					}
				}
			}.template operator ()<Is>(),
			...);
	}(std::make_index_sequence<N>{});

	if (!ok) {
		return std::unexpected(std::move(first_err));
	}
	return {};
}

// Decode a NodeRef into M, handling non-codec integral/float types.
template<class M>
[[nodiscard]] std::expected<M, JsonError> decode_reflect_member(
	NodeRef node,
	JsonDecodeOptions const &opts) {
	if constexpr (is_vector_refl<M>::value) {
		using Elem = typename M::value_type;
		auto arr = node.as_array();
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		M result;
		result.reserve(arr->size());
		for (std::size_t i = 0; i < arr->size(); ++i) {
			auto elem_node = arr->element(i);
			if (!elem_node) {
				return std::unexpected(std::move(elem_node).error());
			}
			auto elem = decode_reflect_member<Elem>(*elem_node, opts);
			if (!elem) {
				JsonPath prefix;
				prefix.push_index(i);
				return std::unexpected(std::move(elem).error().with_prefix(prefix));
			}
			result.push_back(std::move(*elem));
		}
		return result;
	} else if constexpr (is_array_refl<M>::value) {
		using Elem = typename M::value_type;
		constexpr std::size_t N = std::tuple_size_v<M>;
		auto arr = node.as_array();
		if (!arr) {
			return std::unexpected(std::move(arr).error());
		}
		if (arr->size() != N) {
			return std::unexpected(
				JsonError{
					.stage = JsonStage::decode,
					.code = JsonIssueCode::invalid_value,
					.container_size = N,
					.message = std::format("expected array of length {}, got {}", N, arr->size())});
		}
		M result{};
		for (std::size_t i = 0; i < N; ++i) {
			auto elem_node = arr->element(i);
			if (!elem_node) {
				return std::unexpected(std::move(elem_node).error());
			}
			auto elem = decode_reflect_member<Elem>(*elem_node, opts);
			if (!elem) {
				JsonPath prefix;
				prefix.push_index(i);
				return std::unexpected(std::move(elem).error().with_prefix(prefix));
			}
			result[i] = std::move(*elem);
		}
		return result;
	} else if constexpr (has_json_codec<M>) {
		return conflux::json::decode<M>(node, opts);
	} else if constexpr (std::is_signed_v<M> && std::integral<M>) {
		auto r = conflux::json::JsonCodec<std::int64_t>::decode(node);
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
		auto r = conflux::json::JsonCodec<std::uint64_t>::decode(node);
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
		auto r = conflux::json::JsonCodec<double>::decode(node);
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
	if constexpr (requires {
					  conflux::json::JsonCodec<M>::encode(std::declval<ValueBuilder &>(), std::declval<M const &>());
				  }) {
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
// conflux::json::JsonCodec<T> partial specialization — reflection-derived encode / decode
// ---------------------------------------------------------------------------

template<class T>
	requires conflux::json::ReflectJsonAggregate<T>
struct conflux::json::JsonCodec<T> {
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

		constexpr auto N = ::detail::reflect_member_count<T>();

		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(
				[&]<std::size_t I>() {
					if (!ok) {
						return;
					}
					constexpr auto mem = ::detail::reflect_member_at<T, I>();
					if constexpr (::detail::reflect_has_skip<mem>()) {
						return;
					}

					constexpr auto name_info = ::detail::reflect_field_name<mem>();
					std::string_view const field_name{name_info.p, name_info.n};

					using M = std::remove_cvref_t<decltype(result.[:mem:])>;
					auto node = obj.find_member(field_name);
					if (!node) {
						if constexpr (!::detail::is_opt_refl<M>::value) {
							ok = false;
							first_err = JsonError{
								.stage = JsonStage::decode,
								.code = JsonIssueCode::missing_member,
								.member_name = std::string{field_name},
								.message = std::format("missing member: {}", field_name)};
						}
						return;
					}
					auto decoded = ::detail::decode_reflect_member<M>(*node, opts);
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
		// behaves like manual conflux::json::JsonMembers<T> codecs at app/provider boundaries.
		if (opts.unknown_members == UnknownMemberPolicy::reject) {
			for (auto const &m: obj.members()) {
				if (!::detail::is_reflect_member_name<T>(m.name)) {
					ok = false;
					first_err = JsonError{
						.stage = JsonStage::decode,
						.code = JsonIssueCode::invalid_value,
						.member_name = std::string{m.name},
						.message = std::format("unknown member: {}", m.name)};
					break;
				}
			}
		}

		if (!ok) {
			return std::unexpected(std::move(first_err));
		}
		return result;
	}
	template<ParseMode Mode>
	static std::expected<T, JsonError> decode(
		JsonReader &reader,
		JsonReader::Event event,
		JsonDecodeOptions const &opts,
		JsonDecodeScratch *scratch) {
		T result{};
		auto decoded = ::detail::decode_reflect_reader_aggregate_into<Mode>(result, reader, event, opts, scratch);
		if (!decoded) {
			return std::unexpected(std::move(decoded).error());
		}
		return result;
	}
	static std::expected<T, JsonError> decode(
		JsonReader &reader,
		JsonReader::Event event,
		JsonDecodeOptions const &opts,
		JsonDecodeScratch *scratch) {
		if (reader.parse_options().mode == ParseMode::strict) {
			return decode<ParseMode::strict>(reader, event, opts, scratch);
		}
		return decode<ParseMode::json5>(reader, event, opts, scratch);
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

		constexpr auto N = ::detail::reflect_member_count<T>();
		[&]<std::size_t... Is>(std::index_sequence<Is...>) {
			(
				[&]<std::size_t I>() {
					if (!ok) {
						return;
					}
					constexpr auto mem = ::detail::reflect_member_at<T, I>();
					if constexpr (::detail::reflect_has_skip<mem>()) {
						return;
					}

					constexpr auto name_info = ::detail::reflect_field_name<mem>();
					std::string_view const field_name{name_info.p, name_info.n};

					using M = std::remove_cvref_t<decltype(value.[:mem:])>;
					auto res = ::detail::encode_reflect_member<M>(obj, field_name, value.[:mem:]);
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
				conflux::json::dump_detail::dump_string(field_name, out, opts.ascii_only);
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
	} else if constexpr (is_basic_string_refl<Raw>::value) {
		conflux::json::dump_detail::dump_string(std::string_view{value.data(), value.size()}, out, opts.ascii_only);
		return {};
	} else if constexpr (std::same_as<Raw, std::string_view>) {
		conflux::json::dump_detail::dump_string(value, out, opts.ascii_only);
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

export namespace conflux::json {

template<ReflectJsonAggregate T>
std::expected<void, JsonError> write_reflect_json_direct(
	std::string &out,
	T const &value,
	JsonDumpOptions const &opts = {}) {
	return ::detail::reflect_write_object(out, value, opts, 0);
}

template<ReflectJsonAggregate T>
std::expected<std::string, JsonError> dump_reflect_direct(
	T const &value,
	JsonDumpOptions const &opts = {}) {
	std::string out;
	out.reserve(::detail::reflect_member_count<T>() * 16);
	if (auto ok = write_reflect_json_direct(out, value, opts); !ok) {
		return std::unexpected(std::move(ok).error());
	}
	return out;
}

} // namespace conflux::json
